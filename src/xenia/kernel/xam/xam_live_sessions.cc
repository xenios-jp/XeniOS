/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_sessions.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/live_directory.h"
#include "xenia/net/system_link.h"

#include "third_party/fmt/include/fmt/ranges.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

using Clock = std::chrono::steady_clock;

// The System Link port the announcements use, apart from any a title binds.
constexpr uint16_t kPort = 0xFE1A;
constexpr std::array<uint8_t, 4> kMagic = {'X', 'L', 'S', 'D'};
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 4 + 1 + 1 + 4;
constexpr size_t kMaxEntries = 32;

enum Type : uint8_t {
  kAdvert = 1,
  kQuery = 2,
  kWithdraw = 3,
  kArbitration = 4,
};

// How long a machine repeats its arbitration registration for peers that
// register later.
constexpr auto kArbitrationLifetime = std::chrono::seconds(60);

// How often a host repeats its announcements, and how long a search keeps a
// session it stopped hearing about.
constexpr auto kAdvertInterval = std::chrono::milliseconds(1000);
constexpr auto kHeardLifetime = std::chrono::milliseconds(4000);
constexpr uint32_t kReceiveWaitMs = 200;

class Writer {
 public:
  void Bytes(std::span<const uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  void U8(uint8_t v) { out.push_back(v); }
  void U16(uint16_t v) {
    U8(uint8_t(v >> 8));
    U8(uint8_t(v));
  }
  void U32(uint32_t v) {
    U16(uint16_t(v >> 16));
    U16(uint16_t(v));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v >> 32));
    U32(uint32_t(v));
  }
  std::vector<uint8_t> out;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : in_(bytes) {}
  bool Bytes(std::span<uint8_t> bytes) {
    if (in_.size() - pos_ < bytes.size()) {
      return false;
    }
    std::memcpy(bytes.data(), in_.data() + pos_, bytes.size());
    pos_ += bytes.size();
    return true;
  }
  bool U8(uint8_t& v) { return Bytes({&v, 1}); }
  bool U16(uint16_t& v) {
    uint8_t b[2];
    if (!Bytes(b)) return false;
    v = uint16_t(b[0] << 8 | b[1]);
    return true;
  }
  bool U32(uint32_t& v) {
    uint16_t hi, lo;
    if (!U16(hi) || !U16(lo)) return false;
    v = uint32_t(hi) << 16 | lo;
    return true;
  }
  bool U64(uint64_t& v) {
    uint32_t hi, lo;
    if (!U32(hi) || !U32(lo)) return false;
    v = uint64_t(hi) << 32 | lo;
    return true;
  }
  bool done() const { return pos_ == in_.size(); }

 private:
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
};

std::vector<uint8_t> Header(Type type, uint32_t title_id) {
  Writer w;
  w.Bytes(kMagic);
  w.U8(kVersion);
  w.U8(type);
  w.U32(title_id);
  return std::move(w.out);
}

class Directory {
 public:
  ~Directory() {
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
  }

  void Advertise(KernelState* kernel_state, const LiveSessionAdvert& advert) {
    std::lock_guard lock(mutex_);
    if (!StartLocked(kernel_state)) {
      return;
    }
    own_[advert.info.session_id] = advert;
    SendLocked(EncodeLiveSessionAdvert(title_id_, advert));
  }

  void Withdraw(const net::KeyId& id) {
    std::lock_guard lock(mutex_);
    if (!own_.erase(id) || !port_) {
      return;
    }
    auto message = Header(kWithdraw, title_id_);
    message.insert(message.end(), id.begin(), id.end());
    SendLocked(message);
  }

  std::vector<ArbitrationRegistrant> Register(
      KernelState* kernel_state, const net::KeyId& session_id,
      const ArbitrationRegistrant& self, const std::vector<uint64_t>& members,
      uint32_t wait_ms) {
    std::unique_lock lock(mutex_);
    if (!StartLocked(kernel_state)) {
      return {self};
    }
    // TODO(live): every machine builds its own list from what it heard, so a
    // machine that registers after the others stopped waiting is missing from
    // their lists (Halo 3, 4 players, 2026-11-05: 3 lists of 3 machines, 1 of
    // 4; the game still started). Live had one list on its server. Make the
    // session host the authority: it collects registrations for the window
    // and every machine takes its copy. See results/2026-11/live/FINDINGS.md.
    arbitration_[session_id][self.machine_id] = self.xuids;
    Writer w;
    w.out = Header(kArbitration, title_id_);
    w.Bytes(session_id);
    w.U64(self.machine_id);
    w.U16(uint16_t(std::min(self.xuids.size(), kMaxEntries)));
    for (size_t i = 0; i < std::min(self.xuids.size(), kMaxEntries); ++i) {
      w.U64(self.xuids[i]);
    }
    std::erase_if(own_arbitration_, [&](const OwnRegistration& own) {
      return own.session_id == session_id;
    });
    own_arbitration_.push_back(
        {session_id, w.out, Clock::now() + kArbitrationLifetime});
    SendLocked(w.out);
    auto covered = [&] {
      std::set<uint64_t> heard;
      for (const auto& [machine, xuids] : arbitration_[session_id]) {
        heard.insert(xuids.begin(), xuids.end());
      }
      return std::ranges::all_of(
          members, [&](uint64_t xuid) { return heard.contains(xuid); });
    };
    cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), covered);
    std::vector<ArbitrationRegistrant> result;
    for (const auto& [machine, xuids] : arbitration_[session_id]) {
      result.push_back({machine, xuids});
    }
    std::string registered;
    for (const auto& registrant : result) {
      registered += fmt::format(" [{:016X}: {:016X}]", registrant.machine_id,
                                fmt::join(registrant.xuids, " "));
    }
    XELOGI("Live sessions: arbitration with {} machines, {}; members {:016X}; "
           "registered{}",
           result.size(), covered() ? "every member registered" : "timed out",
           fmt::join(members, " "), registered);
    return result;
  }

  std::vector<LiveSessionAdvert> Find(KernelState* kernel_state,
                                      uint32_t wait_ms) {
    std::unique_lock lock(mutex_);
    if (!StartLocked(kernel_state)) {
      return {};
    }
    PruneLocked();
    SendLocked(Header(kQuery, title_id_));
    if (heard_.empty() && wait_ms) {
      cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                   [this] { return !heard_.empty(); });
    }
    std::vector<LiveSessionAdvert> found;
    for (const auto& [id, heard] : heard_) {
      if (!own_.contains(id)) {
        found.push_back(heard.advert);
      }
    }
    return found;
  }

 private:
  struct Heard {
    LiveSessionAdvert advert;
    Clock::time_point seen;
  };
  struct OwnRegistration {
    net::KeyId session_id;
    std::vector<uint8_t> message;
    Clock::time_point expires;
  };

  bool StartLocked(KernelState* kernel_state) {
    auto link = GetSystemLink(kernel_state);
    if (!link) {
      return false;
    }
    if (port_ && link == link_) {
      if (kernel_state->title_id() != title_id_) {
        // Another title runs now: its lobby holds other sessions.
        title_id_ = kernel_state->title_id();
        own_.clear();
        heard_.clear();
        arbitration_.clear();
        own_arbitration_.clear();
      }
      return true;
    }
    // System Link restarted, or this is the first use.
    if (thread_.joinable()) {
      thread_.request_stop();
      mutex_.unlock();
      thread_.join();
      mutex_.lock();
    }
    uint32_t error = 0;
    auto port = link->Bind(kPort, false, error);
    if (!port) {
      XELOGW("Live sessions: no System Link port {:04X}, error {}", kPort,
             error);
      return false;
    }
    link_ = link;
    port_ = std::shared_ptr<net::SystemLinkPort>(std::move(port));
    title_id_ = kernel_state->title_id();
    heard_.clear();
    thread_ = std::jthread(
        [this, port = port_](std::stop_token stop) { Run(stop, port); });
    XELOGI("Live sessions: announcing on the {:08X} lobby", title_id_);
    return true;
  }

  void SendLocked(std::span<const uint8_t> message) {
    if (!port_) {
      return;
    }
    uint32_t error = 0;
    if (port_->Send({0xFFFFFFFF, kPort}, message, error) < 0) {
      XELOGD("Live sessions: send failed, error {}", error);
    }
  }

  void PruneLocked() {
    const auto now = Clock::now();
    std::erase_if(heard_, [now](const auto& entry) {
      return now - entry.second.seen > kHeardLifetime;
    });
  }

  void Run(std::stop_token stop, std::shared_ptr<net::SystemLinkPort> port) {
    std::vector<uint8_t> buffer(net::kMaxDatagramSize);
    auto next_advert = Clock::now();
    while (!stop.stop_requested()) {
      net::Endpoint source;
      uint32_t error = 0;
      const int received =
          port->Receive(buffer, false, source, kReceiveWaitMs, error);
      std::lock_guard lock(mutex_);
      if (received > 0) {
        Handle(std::span(buffer.data(), size_t(received)));
      }
      if (Clock::now() >= next_advert) {
        next_advert = Clock::now() + kAdvertInterval;
        for (const auto& [id, advert] : own_) {
          SendLocked(EncodeLiveSessionAdvert(title_id_, advert));
        }
        const auto now = Clock::now();
        std::erase_if(own_arbitration_, [now](const OwnRegistration& own) {
          return now > own.expires;
        });
        for (const auto& own : own_arbitration_) {
          SendLocked(own.message);
        }
      }
      if (received < 0 && error != net::kWsaTimedOut &&
          error != net::kWsaWouldBlock) {
        // The LAN went down. Wait for the next search or announcement to
        // start again.
        port_.reset();
        return;
      }
    }
  }

  // With mutex_ held.
  void Handle(std::span<const uint8_t> message) {
    if (message.size() < kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), message.begin()) ||
        message[4] != kVersion) {
      return;
    }
    const uint8_t type = message[5];
    const uint32_t title_id = xe::load_and_swap<uint32_t>(message.data() + 6);
    if (title_id != title_id_) {
      return;
    }
    switch (type) {
      case kAdvert: {
        uint32_t ignored;
        LiveSessionAdvert advert;
        if (!DecodeLiveSessionAdvert(message, ignored, advert) ||
            own_.contains(advert.info.session_id)) {
          return;
        }
        const bool fresh = !heard_.contains(advert.info.session_id);
        heard_[advert.info.session_id] = {std::move(advert), Clock::now()};
        if (fresh) {
          XELOGI("Live sessions: heard a session, {} sessions known",
                 heard_.size());
        }
        cv_.notify_all();
        return;
      }
      case kQuery:
        for (const auto& [id, advert] : own_) {
          SendLocked(EncodeLiveSessionAdvert(title_id_, advert));
        }
        return;
      case kArbitration: {
        Reader r(message.subspan(kHeaderSize));
        net::KeyId id;
        uint64_t machine;
        uint16_t count;
        if (!r.Bytes(id) || !r.U64(machine) || !r.U16(count) ||
            count > kMaxEntries) {
          return;
        }
        std::vector<uint64_t> xuids(count);
        for (auto& xuid : xuids) {
          if (!r.U64(xuid)) {
            return;
          }
        }
        auto& registered = arbitration_[id];
        const bool fresh = !registered.contains(machine);
        registered[machine] = std::move(xuids);
        if (fresh) {
          // A machine that registered later has not heard this one yet.
          for (const auto& own : own_arbitration_) {
            if (own.session_id == id) {
              SendLocked(own.message);
            }
          }
        }
        cv_.notify_all();
        return;
      }
      case kWithdraw: {
        if (message.size() != kHeaderSize + sizeof(net::KeyId)) {
          return;
        }
        net::KeyId id;
        std::memcpy(id.data(), message.data() + kHeaderSize, id.size());
        heard_.erase(id);
        return;
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::shared_ptr<net::SystemLink> link_;
  std::shared_ptr<net::SystemLinkPort> port_;
  std::jthread thread_;
  uint32_t title_id_ = 0;
  std::map<net::KeyId, LiveSessionAdvert> own_;
  std::map<net::KeyId, Heard> heard_;
  // Machine ID to players, per ranked session.
  std::map<net::KeyId, std::map<uint64_t, std::vector<uint64_t>>>
      arbitration_;
  std::vector<OwnRegistration> own_arbitration_;
};

Directory& GetDirectory() {
  static Directory directory;
  return directory;
}


// Through the relay: the lobby's session directory, and the sessions this
// machine lists in it.
class OnlineSessions {
 public:
  // Nothing when System Link doesn't go through the relay.
  net::live::SessionDirectory* Directory(KernelState* kernel_state) {
    const std::string lobby = LiveSessionLobby();
    std::lock_guard lock(mutex_);
    if (lobby.empty()) {
      return nullptr;
    }
    const uint32_t title_id = kernel_state->title_id();
    if (!directory_ || lobby != lobby_ || title_id != title_id_) {
      directory_ =
          std::make_unique<net::live::SessionDirectory>(lobby, title_id);
      lobby_ = lobby;
      title_id_ = title_id;
      listed_.clear();
    }
    return directory_.get();
  }

  void Listed(const std::string& id) {
    std::lock_guard lock(mutex_);
    listed_.insert(id);
  }

  // The directory, when it lists |id|; forgets |id|.
  net::live::SessionDirectory* Unlist(const std::string& id) {
    std::lock_guard lock(mutex_);
    return listed_.erase(id) ? directory_.get() : nullptr;
  }

  static std::string Key(const net::KeyId& session_id) {
    uint8_t id[8];
    std::memcpy(id, session_id.data(), sizeof(id));
    return net::live::HexId(id);
  }

 private:
  std::mutex mutex_;
  std::unique_ptr<net::live::SessionDirectory> directory_;
  std::string lobby_;
  uint32_t title_id_ = 0;
  std::set<std::string> listed_;
};

OnlineSessions& GetOnlineSessions() {
  static OnlineSessions sessions;
  return sessions;
}

net::live::DirectoryAdvert ToDirectory(const LiveSessionAdvert& advert) {
  net::live::DirectoryAdvert out;
  out.id = OnlineSessions::Key(advert.info.session_id);
  const auto* info = reinterpret_cast<const uint8_t*>(&advert.info);
  out.info.assign(info, info + sizeof(advert.info));
  // Empty: the title's public lobby, where its players already are.
  out.flags = advert.flags;
  out.public_slots = advert.max_public_slots;
  out.private_slots = advert.max_private_slots;
  out.used_public = advert.used_public_slots;
  out.used_private = advert.used_private_slots;
  out.playing = advert.in_game;
  // Adverts of sessions in game are only made when they can be joined.
  out.join_in_progress = advert.in_game;
  out.xuid = advert.host_xuid;
  for (const auto& context : advert.contexts) {
    out.contexts.emplace_back(context.id, context.value);
  }
  for (const auto& property : advert.properties) {
    out.props.emplace_back(property.id, std::vector<uint8_t>(
                                            property.data.begin(),
                                            property.data.end()));
  }
  return out;
}

std::optional<LiveSessionAdvert> FromDirectory(
    const net::live::DirectoryAdvert& advert) {
  LiveSessionAdvert out;
  if (advert.info.size() != sizeof(out.info)) {
    return std::nullopt;
  }
  std::memcpy(&out.info, advert.info.data(), sizeof(out.info));
  out.flags = advert.flags;
  out.max_public_slots = advert.public_slots;
  out.max_private_slots = advert.private_slots;
  out.used_public_slots = advert.used_public;
  out.used_private_slots = advert.used_private;
  out.host_xuid = advert.xuid;
  out.in_game = advert.playing;
  for (const auto& [id, value] : advert.contexts) {
    out.contexts.push_back({id, value});
  }
  for (const auto& [id, data] : advert.props) {
    LiveSessionAdvert::Property property{id, {}};
    if (data.size() != property.data.size()) {
      continue;
    }
    std::memcpy(property.data.data(), data.data(), data.size());
    out.properties.push_back(property);
  }
  return out;
}

}  // namespace

std::vector<uint8_t> EncodeLiveSessionAdvert(uint32_t title_id,
                                             const LiveSessionAdvert& advert) {
  Writer w;
  w.out = Header(kAdvert, title_id);
  w.Bytes(advert.info.session_id);
  w.Bytes(advert.info.host_address);
  w.Bytes(advert.info.key_exchange_key);
  w.U32(advert.flags);
  w.U32(advert.max_public_slots);
  w.U32(advert.max_private_slots);
  w.U32(advert.used_public_slots);
  w.U32(advert.used_private_slots);
  w.U64(advert.host_xuid);
  const size_t contexts = std::min(advert.contexts.size(), kMaxEntries);
  const size_t properties = std::min(advert.properties.size(), kMaxEntries);
  w.U16(uint16_t(contexts));
  w.U16(uint16_t(properties));
  for (size_t i = 0; i < contexts; ++i) {
    w.U32(advert.contexts[i].id);
    w.U32(advert.contexts[i].value);
  }
  for (size_t i = 0; i < properties; ++i) {
    w.U32(advert.properties[i].id);
    w.Bytes(advert.properties[i].data);
  }
  return std::move(w.out);
}

bool DecodeLiveSessionAdvert(std::span<const uint8_t> bytes,
                             uint32_t& title_id, LiveSessionAdvert& advert) {
  Reader r(bytes);
  std::array<uint8_t, 4> magic;
  uint8_t version, type;
  if (!r.Bytes(magic) || magic != kMagic || !r.U8(version) ||
      version != kVersion || !r.U8(type) || type != kAdvert ||
      !r.U32(title_id)) {
    return false;
  }
  uint16_t contexts, properties;
  if (!r.Bytes(advert.info.session_id) || !r.Bytes(advert.info.host_address) ||
      !r.Bytes(advert.info.key_exchange_key) || !r.U32(advert.flags) ||
      !r.U32(advert.max_public_slots) || !r.U32(advert.max_private_slots) ||
      !r.U32(advert.used_public_slots) || !r.U32(advert.used_private_slots) ||
      !r.U64(advert.host_xuid) || !r.U16(contexts) || !r.U16(properties) ||
      contexts > kMaxEntries || properties > kMaxEntries) {
    return false;
  }
  advert.contexts.resize(contexts);
  for (auto& context : advert.contexts) {
    if (!r.U32(context.id) || !r.U32(context.value)) {
      return false;
    }
  }
  advert.properties.resize(properties);
  for (auto& property : advert.properties) {
    if (!r.U32(property.id) || !r.Bytes(property.data)) {
      return false;
    }
  }
  return r.done();
}

void AdvertiseLiveSession(KernelState* kernel_state,
                          const LiveSessionAdvert& advert) {
  auto& online = GetOnlineSessions();
  if (auto* directory = online.Directory(kernel_state)) {
    auto listing = ToDirectory(advert);
    online.Listed(listing.id);
    directory->Advertise(listing);
    return;
  }
  GetDirectory().Advertise(kernel_state, advert);
}

void WithdrawLiveSession(const net::KeyId& session_id) {
  const std::string id = OnlineSessions::Key(session_id);
  if (auto* directory = GetOnlineSessions().Unlist(id)) {
    directory->Withdraw(id);
    return;
  }
  GetDirectory().Withdraw(session_id);
}

std::vector<ArbitrationRegistrant> RegisterArbitration(
    KernelState* kernel_state, const net::KeyId& session_id,
    const ArbitrationRegistrant& self, const std::vector<uint64_t>& members,
    uint32_t wait_ms) {
  return GetDirectory().Register(kernel_state, session_id, self, members,
                                 wait_ms);
}

std::vector<LiveSessionAdvert> FindLiveSessions(KernelState* kernel_state,
                                                uint32_t wait_ms) {
  auto& online = GetOnlineSessions();
  if (auto* directory = online.Directory(kernel_state)) {
    const auto adverts = directory->Search({}, 50, true, wait_ms);
    std::vector<LiveSessionAdvert> sessions;
    for (const auto& advert : adverts) {
      if (auto session = FromDirectory(advert)) {
        sessions.push_back(std::move(*session));
      }
    }
    return sessions;
  }
  return GetDirectory().Find(kernel_state, wait_ms);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
