/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

/**
 * DXBC to DXIL converter implementation
 * Uses the in-process dxilconv library on macOS.
 */

#include "dxbc_to_dxil_converter.h"

#include <TargetConditionals.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "third_party/xxhash/xxhash.h"
#include "xenia/base/logging.h"

// dxilconv (libdxilconv) ships only as device/macOS binaries; there is no
// iOS-simulator slice. On the simulator the DXBC->DXIL calls are compiled out
// and the Metal backend uses the native MSL path instead.
#if !TARGET_OS_SIMULATOR
#include "DxbcConverter.h"
#endif

#if !defined(_WIN32) && defined(__EMULATE_UUID)
size_t UuidStrHash(const char* key) {
  long hash = 0;
  while (*key) {
    hash = (hash << 4) + *(key++);
    long high_bits = hash & 0xF0000000L;
    if (high_bits) {
      hash ^= high_bits >> 24;
    }
    hash &= ~high_bits;
  }
  return static_cast<size_t>(hash);
}

DEFINE_CROSS_PLATFORM_UUIDOF(IUnknown)
#endif

namespace xe {
namespace gpu {
namespace metal {

#if !TARGET_OS_SIMULATOR

namespace {
constexpr wchar_t kDefaultExtraOptions[] = L"-skip-container-parts";

const CLSID kClsidDxbcConverter = {
    0x4900391e,
    0xb752,
    0x4edd,
    {0xa8, 0x85, 0x6f, 0xb7, 0x6e, 0x25, 0xad, 0xdb}};
std::wstring WidenAscii(const std::string& value) {
  std::wstring out;
  out.reserve(value.size());
  for (char c : value) {
    out.push_back(static_cast<wchar_t>(c));
  }
  return out;
}

std::string HResultHex(HRESULT hr) {
  char buffer[11];
  std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned>(hr));
  return std::string(buffer);
}

struct ThreadConverter {
  IDxbcConverter* converter = nullptr;
  ~ThreadConverter() {
    if (converter) {
      converter->Release();
    }
  }
};
}  // namespace

DxbcToDxilConverter::DxbcToDxilConverter() = default;

DxbcToDxilConverter::~DxbcToDxilConverter() = default;

uint64_t DxbcToDxilConverter::GetOptionsHash() const {
  if (extra_options_.empty()) {
    return 0;
  }
  return XXH3_64bits(extra_options_.data(),
                     extra_options_.size() * sizeof(wchar_t));
}

bool DxbcToDxilConverter::Initialize() {
  const char* extra_options = std::getenv("XENIA_DXBC2DXIL_FLAGS");
  if (extra_options) {
    extra_options_ = WidenAscii(extra_options);
  } else {
    extra_options_ = kDefaultExtraOptions;
  }

  IDxbcConverter* test_converter = nullptr;
  HRESULT hr = DxcCreateInstance(kClsidDxbcConverter, __uuidof(IDxbcConverter),
                                 reinterpret_cast<void**>(&test_converter));
  if (hr != S_OK || !test_converter) {
    XELOGE("DxbcToDxilConverter: Failed to create IDxbcConverter (hr=0x{:08X})",
           static_cast<unsigned>(hr));
    is_available_ = false;
    return false;
  }
  test_converter->Release();

  is_available_ = true;
  if (extra_options && *extra_options) {
    XELOGI("DxbcToDxilConverter: Using extra options: {}", extra_options);
  } else if (extra_options && !*extra_options) {
    XELOGI("DxbcToDxilConverter: Extra options disabled via env");
  } else {
    XELOGI(
        "DxbcToDxilConverter: Using default extra options: "
        "-skip-container-parts");
  }
  return true;
}

bool DxbcToDxilConverter::Convert(const std::vector<uint8_t>& dxbc_data,
                                  std::vector<uint8_t>& dxil_data_out,
                                  std::string* error_message) {
  if (!is_available_) {
    if (error_message) {
      *error_message =
          "DxbcToDxilConverter not initialized or dxilconv unavailable";
    }
    return false;
  }

  // Validate DXBC header
  if (dxbc_data.size() < 4 || dxbc_data[0] != 'D' || dxbc_data[1] != 'X' ||
      dxbc_data[2] != 'B' || dxbc_data[3] != 'C') {
    if (error_message) {
      *error_message = "Invalid DXBC data - missing DXBC magic header";
    }
    return false;
  }

  // Check for debug output directories from environment
  const char* dxbc_dir = std::getenv("XENIA_DXBC_OUTPUT_DIR");
  const char* dxil_dir = std::getenv("XENIA_DXIL_OUTPUT_DIR");

  // Generate unique shader ID based on data hash
  uint64_t hash = 0;
  const size_t hash_bytes = std::min(dxbc_data.size(), size_t(64));
  for (size_t i = 0; i < hash_bytes; ++i) {
    hash = hash * 31 + dxbc_data[i];
  }
  std::string shader_id = std::to_string(hash) + "_" + std::to_string(getpid());

  // Save DXBC to debug directory if requested.
  if (dxbc_dir) {
    std::string debug_input =
        std::string(dxbc_dir) + "/shader_" + shader_id + ".dxbc";
    WriteFile(debug_input, dxbc_data);
  }

  IDxbcConverter* converter = GetThreadConverter(error_message);
  if (!converter) {
    return false;
  }

  void* dxil_ptr = nullptr;
  UINT32 dxil_size = 0;
  wchar_t* diag = nullptr;

  HRESULT hr = converter->Convert(
      dxbc_data.data(), static_cast<UINT32>(dxbc_data.size()),
      extra_options_.empty() ? nullptr : extra_options_.c_str(), &dxil_ptr,
      &dxil_size, &diag);

  if (hr != S_OK || dxil_ptr == nullptr || dxil_size == 0) {
    if (error_message) {
      if (diag) {
        std::string diag_utf8;
        for (const wchar_t* p = diag; *p; ++p) {
          diag_utf8.push_back(static_cast<char>(*p));
        }
        *error_message = "dxbc2dxil failed: " + diag_utf8;
      } else {
        *error_message = "dxbc2dxil failed with HRESULT 0x" + HResultHex(hr);
      }
    }
    CoTaskMemFree(diag);
    CoTaskMemFree(dxil_ptr);
    return false;
  }

  dxil_data_out.assign(reinterpret_cast<const uint8_t*>(dxil_ptr),
                       reinterpret_cast<const uint8_t*>(dxil_ptr) + dxil_size);

  CoTaskMemFree(diag);
  CoTaskMemFree(dxil_ptr);

  // Copy to debug directory if specified.
  if (dxil_dir) {
    std::string debug_output =
        std::string(dxil_dir) + "/shader_" + shader_id + ".dxil";
    WriteFile(debug_output, dxil_data_out);
  }

  // Validate DXIL header (DXBC magic for container, or DXIL for raw).
  if (dxil_data_out.size() < 4) {
    if (error_message) {
      *error_message = "Output DXIL blob too small";
    }
    return false;
  }

  XELOGD(
      "DxbcToDxilConverter: Successfully converted {} bytes DXBC to {} bytes "
      "DXIL",
      dxbc_data.size(), dxil_data_out.size());

  return true;
}

IDxbcConverter* DxbcToDxilConverter::GetThreadConverter(
    std::string* error_message) {
  static thread_local ThreadConverter thread_state;
  if (thread_state.converter) {
    return thread_state.converter;
  }

  HRESULT hr =
      DxcCreateInstance(kClsidDxbcConverter, __uuidof(IDxbcConverter),
                        reinterpret_cast<void**>(&thread_state.converter));
  if (hr != S_OK || !thread_state.converter) {
    if (error_message) {
      *error_message =
          "Failed to create IDxbcConverter (HRESULT 0x" + HResultHex(hr) + ")";
    }
    return nullptr;
  }
  return thread_state.converter;
}

bool DxbcToDxilConverter::WriteFile(const std::string& path,
                                    const std::vector<uint8_t>& data) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }

  file.write(reinterpret_cast<const char*>(data.data()), data.size());
  return file.good();
}

#else  // TARGET_OS_SIMULATOR

// iOS-simulator stub: dxilconv is unavailable, so the converter reports itself
// unavailable and the Metal backend uses the native MSL path.
DxbcToDxilConverter::DxbcToDxilConverter() = default;
DxbcToDxilConverter::~DxbcToDxilConverter() = default;

uint64_t DxbcToDxilConverter::GetOptionsHash() const { return 0; }

bool DxbcToDxilConverter::Initialize() {
  is_available_ = false;
  return false;
}

bool DxbcToDxilConverter::Convert(const std::vector<uint8_t>&,
                                  std::vector<uint8_t>&,
                                  std::string* error_message) {
  if (error_message) {
    *error_message = "dxilconv unavailable (iOS simulator build)";
  }
  return false;
}

IDxbcConverter* DxbcToDxilConverter::GetThreadConverter(std::string*) {
  return nullptr;
}

bool DxbcToDxilConverter::WriteFile(const std::string&,
                                    const std::vector<uint8_t>&) {
  return false;
}

#endif  // TARGET_OS_SIMULATOR

}  // namespace metal
}  // namespace gpu
}  // namespace xe
