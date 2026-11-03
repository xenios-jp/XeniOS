/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/relay_socket.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <optional>

#include <asio.hpp>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/platform.h"

namespace xe::net::relay {
namespace {

using udp = asio::ip::udp;

Endpoint FromAsio(const udp::endpoint& endpoint) {
  Endpoint result;
  result.port = endpoint.port();
  asio::ip::address address = endpoint.address();
  if (address.is_v6() && address.to_v6().is_v4_mapped()) {
    address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
  }
  if (address.is_v4()) {
    const auto bytes = address.to_v4().to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.address.begin());
  } else {
    result.v6 = true;
    const auto bytes = address.to_v6().to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.address.begin());
  }
  return result;
}

std::optional<udp::endpoint> ToAsio(const Endpoint& endpoint, bool v6_socket) {
  if (!endpoint.v6) {
    asio::ip::address_v4::bytes_type bytes;
    std::copy_n(endpoint.address.begin(), bytes.size(), bytes.begin());
    const asio::ip::address_v4 v4(bytes);
    if (v6_socket) {
      return udp::endpoint(asio::ip::make_address_v6(asio::ip::v4_mapped, v4),
                           endpoint.port);
    }
    return udp::endpoint(v4, endpoint.port);
  }
  if (!v6_socket) {
    return std::nullopt;
  }
  asio::ip::address_v6::bytes_type bytes;
  std::copy_n(endpoint.address.begin(), bytes.size(), bytes.begin());
  return udp::endpoint(asio::ip::address_v6(bytes), endpoint.port);
}

// ICMP errors for earlier sends, nothing wrong with the socket.
bool Transient(const asio::error_code& ec) {
#if XE_PLATFORM_WIN32
  if (ec.category() == asio::system_category() &&
      (ec.value() == ERROR_NETWORK_UNREACHABLE ||
       ec.value() == ERROR_HOST_UNREACHABLE)) {
    return true;
  }
#endif
  return ec == asio::error::connection_reset ||
         ec == asio::error::connection_refused ||
         ec == asio::error::network_reset ||
         ec == asio::error::network_unreachable ||
         ec == asio::error::host_unreachable ||
         ec == asio::error::address_family_not_supported ||
         ec == asio::error::no_buffer_space;
}

}  // namespace

struct RelaySocket::Impl {
  asio::io_context io;
  udp::socket socket{io};
  udp::resolver resolver{io};
  bool v6 = false;
  std::vector<uint8_t> buffer = std::vector<uint8_t>(65536);
  std::atomic<bool> woken{false};
};

RelaySocket::RelaySocket() : impl_(std::make_unique<Impl>()) {}

RelaySocket::~RelaySocket() = default;

bool RelaySocket::Open(uint16_t port, std::string& error) {
  asio::error_code ec;
  auto& socket = impl_->socket;
  socket.open(udp::v6(), ec);
  if (!ec) {
    socket.set_option(asio::ip::v6_only(false), ec);
  }
  if (!ec) {
    socket.bind({asio::ip::address_v6::any(), port}, ec);
  }
  impl_->v6 = !ec;
  if (ec) {
    asio::error_code ignored;
    socket.close(ignored);
    ec.clear();
    socket.open(udp::v4(), ec);
    if (!ec) {
      socket.bind({asio::ip::address_v4::any(), port}, ec);
    }
  }
  if (ec) {
    error = fmt::format("UDP port {}: {}", port, ec.message());
    return false;
  }
  // A relay gets bursts from many members at once.
  socket.set_option(asio::socket_base::receive_buffer_size(1 << 20), ec);
  return true;
}

uint16_t RelaySocket::port() const {
  asio::error_code ec;
  return impl_->socket.local_endpoint(ec).port();
}

bool RelaySocket::SendTo(std::span<const uint8_t> data, const Endpoint& to) {
  auto destination = ToAsio(to, impl_->v6);
  if (!destination) {
    return true;
  }
  asio::error_code ec;
  impl_->socket.send_to(asio::buffer(data.data(), data.size()), *destination, 0,
                        ec);
  return !ec || Transient(ec) || ec == asio::error::would_block;
}

RelaySocket::Wait RelaySocket::ReceiveFor(uint32_t timeout_ms,
                                          std::span<const uint8_t>& data,
                                          Endpoint& from, std::string& error) {
  if (impl_->woken.exchange(false)) {
    return Wait::kWoken;
  }
  std::optional<asio::error_code> result;
  size_t size = 0;
  udp::endpoint sender;
  impl_->socket.async_receive_from(
      asio::buffer(impl_->buffer), sender,
      [&](const asio::error_code& ec, size_t received) {
        result = ec;
        size = received;
      });
  impl_->io.restart();
  impl_->io.run_for(std::chrono::milliseconds(timeout_ms));
  if (!result) {
    asio::error_code ignored;
    impl_->socket.cancel(ignored);
    impl_->io.run();
  }
  if (*result == asio::error::operation_aborted) {
    return impl_->woken.exchange(false) ? Wait::kWoken : Wait::kTimeout;
  }
  if (*result && Transient(*result)) {
    return Wait::kTimeout;
  }
  if (*result) {
    error = result->message();
    return Wait::kFailed;
  }
  data = std::span(impl_->buffer.data(), size);
  from = FromAsio(sender);
  return Wait::kDatagram;
}

void RelaySocket::Resolve(const std::string& host, uint16_t port,
                          Resolved done) {
  impl_->resolver.async_resolve(
      host, std::to_string(port),
      udp::resolver::numeric_service | udp::resolver::address_configured,
      [this, done = std::move(done)](const asio::error_code& ec,
                                     udp::resolver::results_type results) {
        if (ec) {
          done({}, ec.message());
          return;
        }
        std::vector<Endpoint> endpoints;
        for (const auto& entry : results) {
          const Endpoint endpoint = FromAsio(entry.endpoint());
          if ((!endpoint.v6 || impl_->v6) &&
              std::find(endpoints.begin(), endpoints.end(), endpoint) ==
                  endpoints.end()) {
            endpoints.push_back(endpoint);
          }
        }
        std::string error = endpoints.empty() ? "no usable address" : "";
        done(std::move(endpoints), std::move(error));
      });
}

void RelaySocket::Wake() {
  impl_->woken = true;
  asio::post(impl_->io, [this] {
    asio::error_code ec;
    impl_->socket.cancel(ec);
  });
}

bool ParseServer(std::string_view text, uint16_t default_port,
                 std::string& host, uint16_t& port) {
  port = default_port;
  std::string_view rest;
  if (text.starts_with('[')) {
    const size_t close = text.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    host = std::string(text.substr(1, close - 1));
    rest = text.substr(close + 1);
  } else if (std::count(text.begin(), text.end(), ':') > 1) {
    // A bare IPv6 address has no port.
    host = std::string(text);
  } else {
    const size_t colon = text.find(':');
    host = std::string(text.substr(0, colon));
    if (colon != std::string_view::npos) {
      rest = text.substr(colon);
    }
  }
  if (!rest.empty()) {
    if (rest[0] != ':' || rest.size() == 1) {
      return false;
    }
    rest.remove_prefix(1);
    uint16_t parsed = 0;
    auto [end, ec] =
        std::from_chars(rest.data(), rest.data() + rest.size(), parsed);
    if (ec != std::errc() || end != rest.data() + rest.size() || !parsed) {
      return false;
    }
    port = parsed;
  }
  return !host.empty();
}

}  // namespace xe::net::relay
