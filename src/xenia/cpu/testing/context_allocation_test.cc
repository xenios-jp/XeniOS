/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstddef>
#include <cstdint>

#include "third_party/catch/single_include/catch2/catch.hpp"
#include "xenia/cpu/context_allocation.h"

namespace xe::cpu {

TEST_CASE("PPC context fixed reservation preserves backend prefix", "[context]") {
  for (size_t granularity : {4096u, 16384u}) {
    size_t calls = 0;
    auto result = detail::AllocateContext(
        4096, granularity, true, [&](void* base, size_t size) -> void* {
          REQUIRE(size == granularity + 4096);
          REQUIRE(base != nullptr);
          ++calls;
          return calls == 3 ? base : nullptr;
        });
    REQUIRE(calls == 3);
    REQUIRE(reinterpret_cast<uintptr_t>(result.context) == 0x42E0000000ull);
    REQUIRE_FALSE(result.used_fallback);
  }
}

TEST_CASE("PPC context falls back only after fixed addresses are refused",
          "[context]") {
  for (size_t granularity : {4096u, 16384u}) {
    constexpr uintptr_t fallback_base = 0x123400000;
    size_t fixed_calls = 0;
    size_t fallback_calls = 0;
    auto result = detail::AllocateContext(
        4096, granularity, true, [&](void* base, size_t size) -> void* {
          REQUIRE(size == granularity + 4096);
          if (base) {
            ++fixed_calls;
            return nullptr;
          }
          REQUIRE(fixed_calls == 8192 - 0x40);
          ++fallback_calls;
          return reinterpret_cast<void*>(fallback_base);
        });
    REQUIRE(fallback_calls == 1);
    REQUIRE(result.used_fallback);
    REQUIRE(reinterpret_cast<uintptr_t>(result.context) ==
            fallback_base + granularity);
    REQUIRE((reinterpret_cast<uintptr_t>(result.context) & 63) == 0);
  }
}

TEST_CASE("PPC context total allocation failure remains null", "[context]") {
  size_t calls = 0;
  auto result = detail::AllocateContext(
      4096, 16384, true, [&](void*, size_t) -> void* {
        ++calls;
        return nullptr;
      });
  REQUIRE(calls == 8192 - 0x40 + 1);
  REQUIRE(result.context == nullptr);
  REQUIRE_FALSE(result.used_fallback);
}

TEST_CASE("Fixed-address-only backends never get arbitrary context addresses",
          "[context]") {
  size_t calls = 0;
  auto result = detail::AllocateContext(
      4096, 4096, false, [&](void* base, size_t) -> void* {
        REQUIRE(base != nullptr);
        ++calls;
        return nullptr;
      });
  REQUIRE(calls == 8192 - 0x40);
  REQUIRE(result.context == nullptr);
}

}  // namespace xe::cpu
