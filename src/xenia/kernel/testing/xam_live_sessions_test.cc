/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_sessions.h"

#include <cstring>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xam {
namespace {

LiveSessionAdvert Sample() {
  LiveSessionAdvert advert;
  for (size_t i = 0; i < advert.info.session_id.size(); ++i) {
    advert.info.session_id[i] = uint8_t(0x10 + i);
  }
  advert.info.host_address[3] = 0x02;
  advert.info.host_address[35] = 0x7F;
  advert.info.key_exchange_key[0] = 0xAB;
  advert.flags = 0x3D;
  advert.max_public_slots = 16;
  advert.max_private_slots = 2;
  advert.used_public_slots = 3;
  advert.used_private_slots = 1;
  advert.host_xuid = 0x0009000044F158E9ull;
  advert.contexts = {{0x800A, 0}, {0x800B, 13}};
  LiveSessionAdvert::Property property{0x10000015, {}};
  property.data[0] = 1;
  property.data[15] = 0xFF;
  advert.properties = {property};
  return advert;
}

TEST_CASE("Live session adverts round-trip", "[live]") {
  const auto advert = Sample();
  const auto bytes = EncodeLiveSessionAdvert(0x4D5307E6, advert);

  uint32_t title_id = 0;
  LiveSessionAdvert decoded;
  REQUIRE(DecodeLiveSessionAdvert(bytes, title_id, decoded));
  CHECK(title_id == 0x4D5307E6);
  CHECK(decoded.info.session_id == advert.info.session_id);
  CHECK(std::memcmp(&decoded.info, &advert.info, sizeof(advert.info)) == 0);
  CHECK(decoded.flags == advert.flags);
  CHECK(decoded.max_public_slots == 16);
  CHECK(decoded.max_private_slots == 2);
  CHECK(decoded.used_public_slots == 3);
  CHECK(decoded.used_private_slots == 1);
  CHECK(decoded.host_xuid == advert.host_xuid);
  REQUIRE(decoded.contexts.size() == 2);
  CHECK(decoded.contexts[1].id == 0x800B);
  CHECK(decoded.contexts[1].value == 13);
  REQUIRE(decoded.properties.size() == 1);
  CHECK(decoded.properties[0].id == 0x10000015);
  CHECK(decoded.properties[0].data == advert.properties[0].data);
}

TEST_CASE("Live session adverts reject damaged messages", "[live]") {
  const auto bytes = EncodeLiveSessionAdvert(0x4D5307D5, Sample());
  uint32_t title_id = 0;
  LiveSessionAdvert decoded;

  // Cut short, or with a byte left over.
  auto shorter = bytes;
  shorter.pop_back();
  CHECK_FALSE(DecodeLiveSessionAdvert(shorter, title_id, decoded));
  auto longer = bytes;
  longer.push_back(0);
  CHECK_FALSE(DecodeLiveSessionAdvert(longer, title_id, decoded));

  // Another magic, version or message type.
  for (const size_t at : {size_t(0), size_t(4), size_t(5)}) {
    auto damaged = bytes;
    damaged[at] ^= 0xFF;
    CHECK_FALSE(DecodeLiveSessionAdvert(damaged, title_id, decoded));
  }
}

TEST_CASE("Live session adverts cap their entries", "[live]") {
  auto advert = Sample();
  advert.contexts.assign(100, {0x800A, 1});
  const auto bytes = EncodeLiveSessionAdvert(0x534507EE, advert);
  uint32_t title_id = 0;
  LiveSessionAdvert decoded;
  REQUIRE(DecodeLiveSessionAdvert(bytes, title_id, decoded));
  CHECK(decoded.contexts.size() == 32);
}

}  // namespace
}  // namespace xam
}  // namespace kernel
}  // namespace xe
