/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_render_target_cache.h"
#include "xenia/gpu/gpu_flags.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "third_party/stb/stb_image_write.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_128bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_128bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/resolve_full_8bpp_scaled_cs.h"

#include "metal_irconverter_runtime.h"

#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/edram_transfer_shader.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_transfer_spirv_cross.h"
#include "xenia/gpu/spirv_to_dxil_compiler.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(
    metal_allow_gamma_unorm16, false,
    "Allow gamma_render_target_as_unorm16 on Metal despite known issues",
    "Metal");
DEFINE_bool(metal_transfer_fast_divmod, true,
            "Use fast exact div/mod in Metal transfer shaders", "Metal");
DEFINE_bool(metal_transfer_spirv_cross, true,
            "Compile the render target ownership transfer shaders with "
            "SPIRV-Cross to MSL and bind their resources directly, instead of "
            "taking them through spirv_to_dxil and the Metal Shader Converter",
            "Metal");
DEFINE_bool(metal_transfer_sample_mask, true,
            "Draw a multisampled transfer destination one sample per draw, "
            "with the sample index in the push constants and the shader "
            "writing the sample mask, unless the source is multisampled too",
            "Metal");
DEFINE_bool(metal_transfer_in_draw_pass, true,
            "Encode render target ownership transfers at the head of the "
            "guest's own render pass instead of in passes of their own",
            "Metal");
DEFINE_int32(metal_memory_log_rate, 0,
             "Log Metal render target/pipeline/instance buffer sizes every N "
             "frames (0 to disable)",
             "Metal");

namespace xe {
namespace gpu {
namespace metal {

namespace {

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

MTL::ComputePipelineState* CreateComputePipelineFromEmbeddedLibrary(
    MTL::Device* device, const void* metallib_data, size_t metallib_size,
    const char* debug_name, const char* entry_point_name = "entry_xe") {
  if (!device || !metallib_data || !metallib_size) {
    return nullptr;
  }

  NS::Error* error = nullptr;

  dispatch_data_t data = dispatch_data_create(
      metallib_data, metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  MTL::Library* lib = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!lib) {
    XELOGE("Metal: failed to create {} library: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  // XeSL compute entrypoint name used in the embedded metallibs.
  NS::String* fn_name =
      NS::String::string(entry_point_name, NS::UTF8StringEncoding);
  MTL::Function* fn = lib->newFunction(fn_name);
  if (!fn) {
    XELOGE("Metal: {} has no function named {}", debug_name, entry_point_name);
    lib->release();
    return nullptr;
  }

  MTL::ComputePipelineState* pipeline =
      device->newComputePipelineState(fn, &error);
  fn->release();
  lib->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create {} pipeline: {}", debug_name,
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  return pipeline;
}

// Argument buffers and descriptor heaps are bound at their slice offset,
// which MSC requires to be aligned the same way MetalDxilBinder aligns the
// guest path's.
constexpr uint32_t kInternalComputeSliceAlignment = 256;

// Draws covering a multisampled transfer destination one sample at a time need
// one push constant set and one argument buffer per sample of the widest
// destination, each holding the layout's dwords plus the sample index those
// draws append.
constexpr uint32_t kTransferSampleDrawMax = 4;
constexpr uint32_t kTransferPushConstantDwordMax =
    kEdramTransferUsedPushConstantDwordCount + 1;

// Packing formats for transferring host RT contents to the EDRAM buffer.
// Keep numeric values in sync with Metal dump shaders in
// InitializeEdramComputeShaders.
enum class MetalEdramDumpFormat : uint32_t {
  kColorRGBA8 = 0,
  kColorRGB10A2Unorm = 1,
  kColorRGB10A2Float = 2,
  kColorRG16Snorm = 3,
  kColorRG16Float = 4,
  kColorR32Float = 5,
  kColorRGBA16Snorm = 6,
  kColorRGBA16Float = 7,
  kColorRGBA16Unorm = 8,
  kColorRG32Float = 9,
  kDepthD24S8 = 16,
  kDepthD24FS8 = 17,
};

struct DebugColor {
  float r;
  float g;
  float b;
  float a;
};

uint32_t FloatToBits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float BitsToFloat(uint32_t value) {
  float out = 0.0f;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

float HalfToFloat(uint16_t value) {
  uint32_t sign = (value >> 15) & 1u;
  uint32_t exponent = (value >> 10) & 0x1Fu;
  uint32_t mantissa = value & 0x3FFu;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      return sign ? -0.0f : 0.0f;
    }
    float base = float(mantissa) * (1.0f / 1024.0f);
    float result = std::ldexp(base, -14);
    return sign ? -result : result;
  }
  if (exponent == 31u) {
    float inf = std::numeric_limits<float>::infinity();
    return sign ? -inf : inf;
  }
  float base = 1.0f + float(mantissa) * (1.0f / 1024.0f);
  float result = std::ldexp(base, int(exponent) - 15);
  return sign ? -result : result;
}

uint16_t FloatToHalf(float value) {
  uint32_t bits = FloatToBits(value);
  uint32_t sign = (bits >> 16) & 0x8000u;
  int exponent = int((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent <= 0) {
    if (exponent < -10) {
      return uint16_t(sign);
    }
    mantissa |= 0x800000u;
    uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    if ((mantissa >> (shift - 1u)) & 1u) {
      ++half;
    }
    return uint16_t(sign | half);
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7C00u);
  }
  uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u) {
    ++half;
  }
  return uint16_t(sign | half);
}

uint32_t PackUnorm(float value, float scale) {
  float clamped = std::min(std::max(value, 0.0f), 1.0f);
  return uint32_t(clamped * scale + 0.5f);
}

uint32_t PackSnorm16(float value) {
  float clamped = std::min(std::max(value, -1.0f), 1.0f);
  float bias = clamped >= 0.0f ? 0.5f : -0.5f;
  int packed = int(clamped * 32767.0f + bias);
  return uint32_t(packed) & 0xFFFFu;
}

uint32_t XePreClampedFloat32To7e3(float value) {
  uint32_t f32 = FloatToBits(value);
  uint32_t biased_f32;
  if (f32 < 0x3E800000u) {
    uint32_t f32_exp = f32 >> 23u;
    uint32_t shift = 125u - f32_exp;
    shift = std::min(shift, 24u);
    uint32_t mantissa = (f32 & 0x7FFFFFu) | 0x800000u;
    biased_f32 = mantissa >> shift;
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  uint32_t round_bit = (biased_f32 >> 16u) & 1u;
  uint32_t f10 = biased_f32 + 0x7FFFu + round_bit;
  return (f10 >> 16u) & 0x3FFu;
}

uint32_t XeUnclampedFloat32To7e3(float value) {
  if (!std::isfinite(value)) {
    value = 0.0f;
  }
  float clamped = std::min(std::max(value, 0.0f), 31.875f);
  return XePreClampedFloat32To7e3(clamped);
}

float XeFloat7e3To32(uint32_t f10) {
  f10 &= 0x3FFu;
  if (!f10) {
    return 0.0f;
  }
  uint32_t mantissa = f10 & 0x7Fu;
  uint32_t exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint32_t lzcnt = 0;
    if (mantissa != 0u) {
      lzcnt = uint32_t(__builtin_clz(mantissa)) - 24u;
    }
    exponent = uint32_t(int32_t(1) - int32_t(lzcnt));
    mantissa = (mantissa << lzcnt) & 0x7Fu;
  }
  uint32_t f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return BitsToFloat(f32);
}

uint32_t PackR8G8B8A8Unorm(const DebugColor& color) {
  uint32_t r = PackUnorm(color.r, 255.0f);
  uint32_t g = PackUnorm(color.g, 255.0f);
  uint32_t b = PackUnorm(color.b, 255.0f);
  uint32_t a = PackUnorm(color.a, 255.0f);
  return r | (g << 8u) | (b << 16u) | (a << 24u);
}

bool PackColor32bpp(uint32_t format, const DebugColor& color,
                    uint32_t* packed_out) {
  switch (format) {
    case uint32_t(MetalEdramDumpFormat::kColorRGBA8): {
      *packed_out = PackR8G8B8A8Unorm(color);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Unorm): {
      uint32_t r = PackUnorm(color.r, 1023.0f);
      uint32_t g = PackUnorm(color.g, 1023.0f);
      uint32_t b = PackUnorm(color.b, 1023.0f);
      uint32_t a = PackUnorm(color.a, 3.0f);
      *packed_out = r | (g << 10u) | (b << 20u) | (a << 30u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Float): {
      uint32_t r = XeUnclampedFloat32To7e3(color.r);
      uint32_t g = XeUnclampedFloat32To7e3(color.g);
      uint32_t b = XeUnclampedFloat32To7e3(color.b);
      uint32_t a = PackUnorm(color.a, 3.0f);
      *packed_out = (r & 0x3FFu) | ((g & 0x3FFu) << 10u) |
                    ((b & 0x3FFu) << 20u) | ((a & 0x3u) << 30u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Snorm): {
      uint32_t r = PackSnorm16(color.r);
      uint32_t g = PackSnorm16(color.g);
      *packed_out = r | (g << 16u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Float): {
      uint16_t r = FloatToHalf(color.r);
      uint16_t g = FloatToHalf(color.g);
      *packed_out = uint32_t(r) | (uint32_t(g) << 16u);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorR32Float): {
      *packed_out = FloatToBits(color.r);
      return true;
    }
    default:
      break;
  }
  return false;
}

bool UnpackColor32bpp(uint32_t format, uint32_t packed, DebugColor* color_out) {
  if (!color_out) {
    return false;
  }
  switch (format) {
    case uint32_t(MetalEdramDumpFormat::kColorRGBA8): {
      color_out->r = float(packed & 0xFFu) * (1.0f / 255.0f);
      color_out->g = float((packed >> 8u) & 0xFFu) * (1.0f / 255.0f);
      color_out->b = float((packed >> 16u) & 0xFFu) * (1.0f / 255.0f);
      color_out->a = float(packed >> 24u) * (1.0f / 255.0f);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Unorm): {
      color_out->r = float(packed & 0x3FFu) * (1.0f / 1023.0f);
      color_out->g = float((packed >> 10u) & 0x3FFu) * (1.0f / 1023.0f);
      color_out->b = float((packed >> 20u) & 0x3FFu) * (1.0f / 1023.0f);
      color_out->a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRGB10A2Float): {
      color_out->r = XeFloat7e3To32(packed & 0x3FFu);
      color_out->g = XeFloat7e3To32((packed >> 10u) & 0x3FFu);
      color_out->b = XeFloat7e3To32((packed >> 20u) & 0x3FFu);
      color_out->a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Snorm): {
      int16_t r = int16_t(packed & 0xFFFFu);
      int16_t g = int16_t(packed >> 16u);
      color_out->r = std::max(float(r) * (1.0f / 32767.0f), -1.0f);
      color_out->g = std::max(float(g) * (1.0f / 32767.0f), -1.0f);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorRG16Float): {
      uint16_t r = uint16_t(packed & 0xFFFFu);
      uint16_t g = uint16_t(packed >> 16u);
      color_out->r = HalfToFloat(r);
      color_out->g = HalfToFloat(g);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case uint32_t(MetalEdramDumpFormat::kColorR32Float): {
      color_out->r = BitsToFloat(packed);
      color_out->g = 0.0f;
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    default:
      break;
  }
  return false;
}

bool DecodeColorTexel(MTL::PixelFormat format, const uint8_t* bytes,
                      DebugColor* color_out) {
  if (!color_out) {
    return false;
  }
  switch (format) {
    case MTL::PixelFormatRGBA16Float: {
      uint16_t components[4];
      std::memcpy(components, bytes, sizeof(components));
      color_out->r = HalfToFloat(components[0]);
      color_out->g = HalfToFloat(components[1]);
      color_out->b = HalfToFloat(components[2]);
      color_out->a = HalfToFloat(components[3]);
      return true;
    }
    case MTL::PixelFormatRG16Float: {
      uint16_t components[2];
      std::memcpy(components, bytes, sizeof(components));
      color_out->r = HalfToFloat(components[0]);
      color_out->g = HalfToFloat(components[1]);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case MTL::PixelFormatRGBA8Unorm: {
      color_out->r = float(bytes[0]) * (1.0f / 255.0f);
      color_out->g = float(bytes[1]) * (1.0f / 255.0f);
      color_out->b = float(bytes[2]) * (1.0f / 255.0f);
      color_out->a = float(bytes[3]) * (1.0f / 255.0f);
      return true;
    }
    case MTL::PixelFormatBGRA8Unorm: {
      color_out->b = float(bytes[0]) * (1.0f / 255.0f);
      color_out->g = float(bytes[1]) * (1.0f / 255.0f);
      color_out->r = float(bytes[2]) * (1.0f / 255.0f);
      color_out->a = float(bytes[3]) * (1.0f / 255.0f);
      return true;
    }
    case MTL::PixelFormatRGB10A2Unorm:
    case MTL::PixelFormatBGR10A2Unorm: {
      uint32_t packed = 0;
      std::memcpy(&packed, bytes, sizeof(packed));
      DebugColor unpacked;
      unpacked.r = float(packed & 0x3FFu) * (1.0f / 1023.0f);
      unpacked.g = float((packed >> 10u) & 0x3FFu) * (1.0f / 1023.0f);
      unpacked.b = float((packed >> 20u) & 0x3FFu) * (1.0f / 1023.0f);
      unpacked.a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
      if (format == MTL::PixelFormatBGR10A2Unorm) {
        std::swap(unpacked.r, unpacked.b);
      }
      *color_out = unpacked;
      return true;
    }
    case MTL::PixelFormatR32Float: {
      uint32_t packed = 0;
      std::memcpy(&packed, bytes, sizeof(packed));
      color_out->r = BitsToFloat(packed);
      color_out->g = 0.0f;
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    case MTL::PixelFormatRG32Float: {
      uint32_t packed[2] = {};
      std::memcpy(packed, bytes, sizeof(packed));
      color_out->r = BitsToFloat(packed[0]);
      color_out->g = BitsToFloat(packed[1]);
      color_out->b = 0.0f;
      color_out->a = 1.0f;
      return true;
    }
    default:
      break;
  }
  return false;
}

size_t MsaaSamplesToIndex(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k1X:
      return 0;
    case xenos::MsaaSamples::k2X:
      return 1;
    case xenos::MsaaSamples::k4X:
      return 2;
    default:
      return 0;
  }
}

uint32_t MsaaSamplesToCount(xenos::MsaaSamples samples) {
  switch (samples) {
    case xenos::MsaaSamples::k1X:
      return 1;
    case xenos::MsaaSamples::k2X:
      return 2;
    case xenos::MsaaSamples::k4X:
      return 4;
    default:
      return 1;
  }
}

// Matches TransferVertexConstants in the transfer library's MSL.
struct TransferVertexConstants {
  float pixel_to_ndc_x;
  float pixel_to_ndc_y;
};

struct TransferRectInstance {
  float origin_x;
  float origin_y;
  float size_x;
  float size_y;
};

struct TransferClearColorFloatConstants {
  float color[4];
};

struct TransferClearColorUintConstants {
  uint32_t color[4];
};

struct TransferClearDepthConstants {
  float depth;
  float padding[3];
};

}  // namespace

// MetalRenderTarget implementation
MetalRenderTargetCache::MetalRenderTarget::~MetalRenderTarget() {
  if (stencil_view_) {
    stencil_view_->release();
    stencil_view_ = nullptr;
  }
  if (draw_texture_ && draw_texture_ != texture_) {
    draw_texture_->release();
    draw_texture_ = nullptr;
  }
  if (transfer_texture_ && transfer_texture_ != texture_) {
    transfer_texture_->release();
    transfer_texture_ = nullptr;
  }
  if (texture_) {
    texture_->release();
    texture_ = nullptr;
  }
}

// MetalRenderTargetCache implementation
MetalRenderTargetCache::MetalRenderTargetCache(
    const RegisterFile& register_file, const Memory& memory,
    TraceWriter* trace_writer, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, MetalCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, trace_writer,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

MetalRenderTargetCache::~MetalRenderTargetCache() { Shutdown(true); }

RenderTargetCache::Path MetalRenderTargetCache::GetPath() const {
  return Path::kHostRenderTargets;
}

bool MetalRenderTargetCache::Initialize() {
  device_ = command_processor_.GetMetalDevice();
  if (!device_) {
    XELOGE("MetalRenderTargetCache: No Metal device available");
    return false;
  }

  // 2x msaa and unorm16 support virtually guarunteed as minimum OS target /
  // Metal version currently is MacOS 15 / Metal 3
  msaa_2x_supported_ = device_->supportsTextureSampleCount(2);

  native_stencil_output_probed_ = ProbeNativeStencilOutputSupport();
  if (!native_stencil_output_probed_) {
    XELOGW(
        "Metal: no pixel shader stencil output on this device; stencil "
        "transfers will use eight masked draws per rectangle");
  }

  gamma_render_target_as_unorm16_ = ::cvars::gamma_render_target_as_unorm16 &&
                                    ::cvars::metal_allow_gamma_unorm16;
  if (::cvars::gamma_render_target_as_unorm16 &&
      !::cvars::metal_allow_gamma_unorm16) {
    XELOGW(
        "Metal: gamma_render_target_as_unorm16 disabled due to known issues; "
        "set --metal_allow_gamma_unorm16=true to force");
  }

  // Create the EDRAM buffer.
  //
  // The guest has 10 MiB of EDRAM for samples, but with host resolution
  // scaling enabled the compute path addresses a scaled EDRAM layout (the
  // shaders multiply the tile dimensions by resolution_scale_x/y). The buffer
  // therefore must be scaled by the same factor to avoid out-of-bounds writes.
  const uint32_t scale_x = std::max<uint32_t>(1u, draw_resolution_scale_x());
  const uint32_t scale_y = std::max<uint32_t>(1u, draw_resolution_scale_y());
  const size_t edram_dwords = size_t(xenos::kEdramTileCount) *
                              size_t(xenos::kEdramTileWidthSamples) *
                              size_t(xenos::kEdramTileHeightSamples) *
                              size_t(scale_x) * size_t(scale_y);
  const size_t edram_size_bytes = edram_dwords * sizeof(uint32_t);
  const bool edram_cpu_visible = false;
  const MTL::ResourceOptions edram_storage_mode =
      edram_cpu_visible ? MTL::ResourceStorageModeShared
                        : MTL::ResourceStorageModePrivate;
  edram_buffer_ = device_->newBuffer(edram_size_bytes, edram_storage_mode);
  if (!edram_buffer_) {
    XELOGE("MetalRenderTargetCache: Failed to create EDRAM buffer");
    return false;
  }
  edram_buffer_->setLabel(
      NS::String::string("EDRAM Buffer", NS::UTF8StringEncoding));
  if (edram_cpu_visible) {
    void* edram_contents = edram_buffer_->contents();
    if (edram_contents) {
      std::memset(edram_contents, 0, edram_size_bytes);
    }
  } else {
    ScopedAutoreleasePool autorelease_pool;
    MTL::CommandBuffer* cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
    if (cmd) {
      MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
      if (blit) {
        blit->fillBuffer(
            edram_buffer_,
            NS::Range::Make(0, static_cast<NS::UInteger>(edram_size_bytes)), 0);
        blit->endEncoding();
        cmd->commit();
      } else {
        command_processor_.DiscardAccountedCommandBuffer(
            cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
      }
    }
  }
  // Initialize EDRAM compute shaders
  if (!InitializeEdramComputeShaders()) {
    XELOGE(
        "MetalRenderTargetCache: Failed to initialize EDRAM compute shaders");
    return false;
  }

  // Initialize base class
  InitializeCommon();

  return true;
}

void MetalRenderTargetCache::Shutdown(bool from_destructor) {
  ClearPendingDrawPassTransfers();
  if (!from_destructor) {
    ClearCache();
  }

  // Clean up dummy target
  dummy_color_targets_.clear();
  dummy_color_target_ = nullptr;
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  for (auto& it : transfer_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_pipelines_.clear();
  for (auto& it : transfer_fragment_functions_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_fragment_functions_.clear();
  for (auto& it : edram_load_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  edram_load_pipelines_.clear();
  for (auto& it : transfer_clear_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_clear_pipelines_.clear();
  if (transfer_library_) {
    transfer_library_->release();
    transfer_library_ = nullptr;
  }
  if (edram_load_library_) {
    edram_load_library_->release();
    edram_load_library_ = nullptr;
  }
  if (edram_load_library_msaa_) {
    edram_load_library_msaa_->release();
    edram_load_library_msaa_ = nullptr;
  }
  if (transfer_depth_state_) {
    transfer_depth_state_->release();
    transfer_depth_state_ = nullptr;
  }
  if (transfer_depth_stencil_output_state_) {
    transfer_depth_stencil_output_state_->release();
    transfer_depth_stencil_output_state_ = nullptr;
  }
  if (transfer_depth_state_none_) {
    transfer_depth_state_none_->release();
    transfer_depth_state_none_ = nullptr;
  }
  if (transfer_depth_clear_state_) {
    transfer_depth_clear_state_->release();
    transfer_depth_clear_state_ = nullptr;
  }
  if (transfer_stencil_clear_state_) {
    transfer_stencil_clear_state_->release();
    transfer_stencil_clear_state_ = nullptr;
  }
  for (auto& state : transfer_stencil_bit_states_) {
    if (state) {
      state->release();
      state = nullptr;
    }
  }
  if (transfer_dummy_buffer_) {
    transfer_dummy_buffer_->release();
    transfer_dummy_buffer_ = nullptr;
  }
  for (size_t i = 0; i < xe::countof(transfer_dummy_color_float_); ++i) {
    if (transfer_dummy_color_float_[i]) {
      transfer_dummy_color_float_[i]->release();
      transfer_dummy_color_float_[i] = nullptr;
    }
    if (transfer_dummy_color_uint_[i]) {
      transfer_dummy_color_uint_[i]->release();
      transfer_dummy_color_uint_[i] = nullptr;
    }
    if (transfer_dummy_depth_[i]) {
      transfer_dummy_depth_[i]->release();
      transfer_dummy_depth_[i] = nullptr;
    }
    if (transfer_dummy_stencil_[i]) {
      transfer_dummy_stencil_[i]->release();
      transfer_dummy_stencil_[i] = nullptr;
    }
  }

  // Clean up EDRAM compute shaders
  ShutdownEdramComputeShaders();

  if (edram_buffer_) {
    edram_buffer_->release();
    edram_buffer_ = nullptr;
  }

  // Destroy all render targets
  DestroyAllRenderTargets(!from_destructor);
  render_target_map_.clear();

  // Shutdown base class
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool MetalRenderTargetCache::InitializeEdramComputeShaders() {
  // Initialize the resolve / EDRAM compute pipelines used by the Metal backend.
  const bool draw_resolution_scaled = IsDrawResolutionScaled();
  edram_load_pipeline_ = nullptr;
  edram_store_pipeline_ = nullptr;
  resolve_full_8bpp_pipeline_ = nullptr;
  resolve_full_16bpp_pipeline_ = nullptr;
  resolve_full_32bpp_pipeline_ = nullptr;
  resolve_full_64bpp_pipeline_ = nullptr;
  resolve_full_128bpp_pipeline_ = nullptr;
  resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  resolve_full_8bpp_scaled_pipeline_ = nullptr;
  resolve_full_16bpp_scaled_pipeline_ = nullptr;
  resolve_full_32bpp_scaled_pipeline_ = nullptr;
  resolve_full_64bpp_scaled_pipeline_ = nullptr;
  resolve_full_128bpp_scaled_pipeline_ = nullptr;
  resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_32bpp_4xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  resolve_fast_64bpp_4xmsaa_scaled_pipeline_ = nullptr;
  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    host_depth_store_pipelines_[i] = nullptr;
  }

  NS::Error* error = nullptr;

  // Resolve compute pipelines.
  resolve_full_8bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_8bpp_cs_metallib,
      sizeof(resolve_full_8bpp_cs_metallib), "resolve_full_8bpp");
  resolve_full_16bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_16bpp_cs_metallib,
      sizeof(resolve_full_16bpp_cs_metallib), "resolve_full_16bpp");
  resolve_full_32bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_32bpp_cs_metallib,
      sizeof(resolve_full_32bpp_cs_metallib), "resolve_full_32bpp");
  resolve_full_64bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_64bpp_cs_metallib,
      sizeof(resolve_full_64bpp_cs_metallib), "resolve_full_64bpp");
  resolve_full_128bpp_pipeline_ = CreateComputePipelineFromEmbeddedLibrary(
      device_, resolve_full_128bpp_cs_metallib,
      sizeof(resolve_full_128bpp_cs_metallib), "resolve_full_128bpp");
  resolve_fast_32bpp_1x2xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_32bpp_1x2xmsaa_cs_metallib,
          sizeof(resolve_fast_32bpp_1x2xmsaa_cs_metallib),
          "resolve_fast_32bpp_1x2xmsaa");
  resolve_fast_32bpp_4xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_32bpp_4xmsaa_cs_metallib,
          sizeof(resolve_fast_32bpp_4xmsaa_cs_metallib),
          "resolve_fast_32bpp_4xmsaa");
  resolve_fast_64bpp_1x2xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_64bpp_1x2xmsaa_cs_metallib,
          sizeof(resolve_fast_64bpp_1x2xmsaa_cs_metallib),
          "resolve_fast_64bpp_1x2xmsaa");
  resolve_fast_64bpp_4xmsaa_pipeline_ =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, resolve_fast_64bpp_4xmsaa_cs_metallib,
          sizeof(resolve_fast_64bpp_4xmsaa_cs_metallib),
          "resolve_fast_64bpp_4xmsaa");

  if (!resolve_full_8bpp_pipeline_ || !resolve_full_16bpp_pipeline_ ||
      !resolve_full_32bpp_pipeline_ || !resolve_full_64bpp_pipeline_ ||
      !resolve_full_128bpp_pipeline_ ||
      !resolve_fast_32bpp_1x2xmsaa_pipeline_ ||
      !resolve_fast_32bpp_4xmsaa_pipeline_ ||
      !resolve_fast_64bpp_1x2xmsaa_pipeline_ ||
      !resolve_fast_64bpp_4xmsaa_pipeline_) {
    XELOGE("Metal: failed to initialize resolve compute pipelines");
    return false;
  }

  if (draw_resolution_scaled) {
    resolve_full_8bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_8bpp_scaled_cs_metallib,
            sizeof(resolve_full_8bpp_scaled_cs_metallib),
            "resolve_full_8bpp_scaled");
    resolve_full_16bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_16bpp_scaled_cs_metallib,
            sizeof(resolve_full_16bpp_scaled_cs_metallib),
            "resolve_full_16bpp_scaled");
    resolve_full_32bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_32bpp_scaled_cs_metallib,
            sizeof(resolve_full_32bpp_scaled_cs_metallib),
            "resolve_full_32bpp_scaled");
    resolve_full_64bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_64bpp_scaled_cs_metallib,
            sizeof(resolve_full_64bpp_scaled_cs_metallib),
            "resolve_full_64bpp_scaled");
    resolve_full_128bpp_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_full_128bpp_scaled_cs_metallib,
            sizeof(resolve_full_128bpp_scaled_cs_metallib),
            "resolve_full_128bpp_scaled");
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_32bpp_1x2xmsaa_scaled_cs_metallib),
            "resolve_fast_32bpp_1x2xmsaa_scaled");
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_32bpp_4xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_32bpp_4xmsaa_scaled_cs_metallib),
            "resolve_fast_32bpp_4xmsaa_scaled");
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_64bpp_1x2xmsaa_scaled_cs_metallib),
            "resolve_fast_64bpp_1x2xmsaa_scaled");
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_ =
        CreateComputePipelineFromEmbeddedLibrary(
            device_, resolve_fast_64bpp_4xmsaa_scaled_cs_metallib,
            sizeof(resolve_fast_64bpp_4xmsaa_scaled_cs_metallib),
            "resolve_fast_64bpp_4xmsaa_scaled");
    if (!resolve_full_8bpp_scaled_pipeline_ ||
        !resolve_full_16bpp_scaled_pipeline_ ||
        !resolve_full_32bpp_scaled_pipeline_ ||
        !resolve_full_64bpp_scaled_pipeline_ ||
        !resolve_full_128bpp_scaled_pipeline_ ||
        !resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ ||
        !resolve_fast_32bpp_4xmsaa_scaled_pipeline_ ||
        !resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ ||
        !resolve_fast_64bpp_4xmsaa_scaled_pipeline_) {
      XELOGE("Metal: failed to initialize scaled resolve compute pipelines");
      return false;
    }
  }

  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_1xmsaa_cs_metallib,
          sizeof(host_depth_store_1xmsaa_cs_metallib),
          "host_depth_store_1xmsaa");
  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_2xmsaa_cs_metallib,
          sizeof(host_depth_store_2xmsaa_cs_metallib),
          "host_depth_store_2xmsaa");
  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_4xmsaa_cs_metallib,
          sizeof(host_depth_store_4xmsaa_cs_metallib),
          "host_depth_store_4xmsaa");

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    if (!host_depth_store_pipelines_[i]) {
      XELOGE("Metal: failed to initialize host depth store pipelines");
      return false;
    }
  }

  return true;
}

void MetalRenderTargetCache::ShutdownEdramComputeShaders() {
  for (auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second) {
      dump_pipeline_pair.second->release();
    }
  }
  dump_pipelines_.clear();
  if (edram_load_pipeline_) {
    edram_load_pipeline_->release();
    edram_load_pipeline_ = nullptr;
  }
  if (edram_store_pipeline_) {
    edram_store_pipeline_->release();
    edram_store_pipeline_ = nullptr;
  }
  // Release 32bpp depth dump pipelines
  // Release resolve pipelines
  if (resolve_full_8bpp_pipeline_) {
    resolve_full_8bpp_pipeline_->release();
    resolve_full_8bpp_pipeline_ = nullptr;
  }
  if (resolve_full_16bpp_pipeline_) {
    resolve_full_16bpp_pipeline_->release();
    resolve_full_16bpp_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_pipeline_) {
    resolve_full_32bpp_pipeline_->release();
    resolve_full_32bpp_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_pipeline_) {
    resolve_full_64bpp_pipeline_->release();
    resolve_full_64bpp_pipeline_ = nullptr;
  }
  if (resolve_full_128bpp_pipeline_) {
    resolve_full_128bpp_pipeline_->release();
    resolve_full_128bpp_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_1x2xmsaa_pipeline_) {
    resolve_fast_32bpp_1x2xmsaa_pipeline_->release();
    resolve_fast_32bpp_1x2xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_4xmsaa_pipeline_) {
    resolve_fast_32bpp_4xmsaa_pipeline_->release();
    resolve_fast_32bpp_4xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_1x2xmsaa_pipeline_) {
    resolve_fast_64bpp_1x2xmsaa_pipeline_->release();
    resolve_fast_64bpp_1x2xmsaa_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_4xmsaa_pipeline_) {
    resolve_fast_64bpp_4xmsaa_pipeline_->release();
    resolve_fast_64bpp_4xmsaa_pipeline_ = nullptr;
  }
  if (resolve_full_8bpp_scaled_pipeline_) {
    resolve_full_8bpp_scaled_pipeline_->release();
    resolve_full_8bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_16bpp_scaled_pipeline_) {
    resolve_full_16bpp_scaled_pipeline_->release();
    resolve_full_16bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_32bpp_scaled_pipeline_) {
    resolve_full_32bpp_scaled_pipeline_->release();
    resolve_full_32bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_64bpp_scaled_pipeline_) {
    resolve_full_64bpp_scaled_pipeline_->release();
    resolve_full_64bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_full_128bpp_scaled_pipeline_) {
    resolve_full_128bpp_scaled_pipeline_->release();
    resolve_full_128bpp_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_) {
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_->release();
    resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_32bpp_4xmsaa_scaled_pipeline_) {
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_->release();
    resolve_fast_32bpp_4xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_) {
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_->release();
    resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_ = nullptr;
  }
  if (resolve_fast_64bpp_4xmsaa_scaled_pipeline_) {
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_->release();
    resolve_fast_64bpp_4xmsaa_scaled_pipeline_ = nullptr;
  }
  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    if (host_depth_store_pipelines_[i]) {
      host_depth_store_pipelines_[i]->release();
      host_depth_store_pipelines_[i] = nullptr;
    }
  }
}

void MetalRenderTargetCache::ClearCache() {
  ClearPendingDrawPassTransfers();

  // Clear current bindings
  for (uint32_t i = 0; i < 4; ++i) {
    current_color_targets_[i] = nullptr;
  }
  current_depth_target_ = nullptr;
  render_pass_descriptor_dirty_ = true;
  render_pass_encoder_created_since_targets_changed_ = false;

  // Clear the tracking of which render targets have been cleared
  cleared_render_targets_this_frame_.clear();
  dummy_color_targets_.clear();
  dummy_color_target_ = nullptr;
  render_target_map_.clear();

  // Call base implementation
  RenderTargetCache::ClearCache();
}

void MetalRenderTargetCache::RenderTargetReused(RenderTarget& render_target,
                                                RenderTargetKey old_key) {
  MetalRenderTarget& metal_render_target =
      static_cast<MetalRenderTarget&>(render_target);
  auto it = render_target_map_.find(old_key.key);
  if (it != render_target_map_.end() && it->second == &metal_render_target) {
    render_target_map_.erase(it);
  }
  render_target_map_[render_target.key().key] = &metal_render_target;
  // Cleared on the first use like a new one.
  metal_render_target.SetNeedsInitialClear(true);
}

bool MetalRenderTargetCache::CanReuseRenderTarget(
    const RenderTarget& render_target) const {
  // Transfers that failed to flush still reference their sources.
  if (HasPendingDrawPassTransfers()) {
    return false;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_color_targets_[i] == &render_target ||
        last_real_color_targets_[i] == &render_target) {
      return false;
    }
  }
  if (current_depth_target_ == &render_target ||
      last_real_depth_target_ == &render_target) {
    return false;
  }
  for (const RenderTarget* pending_render_target :
       pending_draw_pass_render_targets_) {
    if (pending_render_target == &render_target) {
      return false;
    }
  }
  return true;
}

void MetalRenderTargetCache::BeginFrame() {
  SCOPE_profile_cpu_f("gpu");
  (void)FlushPendingDrawPassTransfers();

  ++frame_id_;

  // Clear the tracking of which render targets have been cleared this frame
  cleared_render_targets_this_frame_.clear();

  // Call base implementation
  RenderTargetCache::BeginFrame();

  if (::cvars::metal_memory_log_rate > 0 &&
      (frame_id_ % uint64_t(::cvars::metal_memory_log_rate)) == 0) {
    XELOGI(
        "Metal mem: frame={} rt={} map={} dummy={} pipelines={} "
        "transfer_shaders={}",
        frame_id_, render_target_map_.size(), render_target_map_.size(),
        dummy_color_targets_.size(), transfer_pipelines_.size(),
        transfer_fragment_functions_.size());
  }
}

bool MetalRenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader,
    int32_t window_offset_tiles) {
  SCOPE_profile_cpu_f("gpu");
  // Reaching another update means the command processor never got to encode
  // the queued transfers into a pass. Their ownership is already transferred,
  // so run them standalone before the base update reshuffles ownership again.
  if (!FlushPendingDrawPassTransfers()) {
    return false;
  }

  // Use the base class logic to update the current render target setup.
  if (!RenderTargetCache::Update(
          is_rasterization_done, normalized_depth_control,
          normalized_color_mask, vertex_shader, window_offset_tiles)) {
    XELOGE("MetalRenderTargetCache::Update - Base class Update failed");
    return false;
  }

  if (::cvars::metal_memory_log_rate > 0) {
    static uint64_t memory_log_counter = 0;
    if ((++memory_log_counter % uint64_t(::cvars::metal_memory_log_rate)) ==
        0) {
      XELOGI(
          "Metal mem: frame={} rt={} map={} dummy={} pipelines={} "
          "transfer_shaders={}",
          frame_id_, render_target_map_.size(), render_target_map_.size(),
          dummy_color_targets_.size(), transfer_pipelines_.size(),
          transfer_fragment_functions_.size());
    }
  }

  // After base class update, retrieve the actual render targets that were
  // selected This is the KEY to connecting base class management with
  // Metal-specific rendering
  RenderTarget* const* accumulated_targets =
      last_update_accumulated_render_targets();

  // Check if render targets actually changed
  bool targets_changed = false;

  // Check depth target
  MetalRenderTarget* new_depth_target =
      accumulated_targets[0]
          ? static_cast<MetalRenderTarget*>(accumulated_targets[0])
          : nullptr;
  if (new_depth_target != current_depth_target_) {
    targets_changed = true;
    current_depth_target_ = new_depth_target;
    if (current_depth_target_) {
      XELOGD(
          "MetalRenderTargetCache::Update - Depth target changed: key={:08X}",
          current_depth_target_->key().key);
    }
  }

  // Check color targets
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MetalRenderTarget* new_color_target =
        accumulated_targets[i + 1]
            ? static_cast<MetalRenderTarget*>(accumulated_targets[i + 1])
            : nullptr;
    if (new_color_target != current_color_targets_[i]) {
      targets_changed = true;
      current_color_targets_[i] = new_color_target;
      if (current_color_targets_[i]) {
        XELOGD(
            "MetalRenderTargetCache::Update - Color target {} changed: "
            "key={:08X}",
            i, current_color_targets_[i]->key().key);
      }
    }
  }

  // Perform ownership transfers - this is critical for correct rendering when
  // EDRAM regions are aliased between different RT configurations.
  // The base class Update() populates last_update_transfers() with the needed
  // transfers based on EDRAM tile overlaps.
  const std::vector<Transfer>* update_transfers = last_update_transfers();
  if (::cvars::metal_transfer_in_draw_pass) {
    std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
        fallback_transfers;
    bool fallback_transfer_work = false;
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      const std::vector<Transfer>& transfers = update_transfers[i];
      if (transfers.empty()) {
        continue;
      }
      if (!CanQueueDrawPassTransfers(i, accumulated_targets, transfers)) {
        fallback_transfers[i] = transfers;
        fallback_transfer_work = true;
        continue;
      }
      pending_draw_pass_render_targets_[i] = accumulated_targets[i];
      pending_draw_pass_transfers_[i] = transfers;
      pending_draw_pass_transfer_mask_ |= uint32_t(1) << i;
      if (PendingDrawPassTransfersFullyOverwriteTarget(
              i, accumulated_targets[i], transfers)) {
        pending_draw_pass_full_overwrite_mask_ |= uint32_t(1) << i;
      }
      // The transfer draws supersede the clear the descriptor would have done,
      // matching what the standalone path does with the flag.
      auto* dest_metal_rt =
          static_cast<MetalRenderTarget*>(accumulated_targets[i]);
      if (dest_metal_rt->needs_initial_clear()) {
        dest_metal_rt->SetNeedsInitialClear(false);
        render_pass_descriptor_dirty_ = true;
      }
    }
    if (fallback_transfer_work) {
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       accumulated_targets,
                                       fallback_transfers.data());
    }
    // Queue only what the pass the descriptor will describe can actually
    // encode, so eligibility is not re-decided at encode time.
    if (HasPendingDrawPassTransfers()) {
      TransferAttachmentFormats attachment_formats;
      if (!GetCurrentTransferAttachmentFormats(attachment_formats) ||
          !PreflightPendingDrawPassTransfers(attachment_formats)) {
        if (!FlushPendingDrawPassTransfers()) {
          return false;
        }
      }
    }
  } else {
    PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                     accumulated_targets, update_transfers);
  }

  // Only mark render pass descriptor as dirty if targets actually changed
  if (targets_changed) {
    render_pass_descriptor_dirty_ = true;
    // A different attachment set starts a pass of its own, with nothing an
    // earlier encoder left in these attachments to preserve.
    render_pass_encoder_created_since_targets_changed_ = false;
  }

  return true;
}

void MetalRenderTargetCache::SetCachedRenderPassLoadActions(
    uint32_t attachment_mask, MTL::LoadAction load_action) {
  if (!attachment_mask || !cached_render_pass_descriptor_) {
    return;
  }
  if (attachment_mask & 1) {
    if (auto* depth_attachment =
            cached_render_pass_descriptor_->depthAttachment()) {
      depth_attachment->setLoadAction(load_action);
    }
    if (auto* stencil_attachment =
            cached_render_pass_descriptor_->stencilAttachment()) {
      stencil_attachment->setLoadAction(load_action);
    }
  }
  if (auto* color_attachments =
          cached_render_pass_descriptor_->colorAttachments()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(attachment_mask & (uint32_t(2) << i))) {
        continue;
      }
      if (auto* color_attachment = color_attachments->object(i)) {
        color_attachment->setLoadAction(load_action);
      }
    }
  }
}

uint32_t MetalRenderTargetCache::GetPendingDrawPassLoadDontCareMask() {
  // An encoder already wrote these attachments, so a reopen has to load them.
  if (render_pass_encoder_created_since_targets_changed_ ||
      !HasPendingDrawPassTransfers()) {
    return 0;
  }
  TransferAttachmentFormats attachment_formats;
  if (!GetCurrentTransferAttachmentFormats(attachment_formats) ||
      !PreflightPendingDrawPassTransfers(attachment_formats)) {
    return 0;
  }
  return pending_draw_pass_transfer_mask_ &
         pending_draw_pass_full_overwrite_mask_;
}

void MetalRenderTargetCache::ApplyPendingDrawPassLoadActions() {
  uint32_t dontcare_mask = GetPendingDrawPassLoadDontCareMask();
  SetCachedRenderPassLoadActions(
      pending_draw_pass_load_dontcare_mask_ & ~dontcare_mask,
      MTL::LoadActionLoad);
  SetCachedRenderPassLoadActions(
      dontcare_mask & ~pending_draw_pass_load_dontcare_mask_,
      MTL::LoadActionDontCare);
  pending_draw_pass_load_dontcare_mask_ = dontcare_mask;
}

void MetalRenderTargetCache::ClearPendingDrawPassTransfers() {
  // A DontCare load action only holds for the one pass that also encodes the
  // transfers. Patch the descriptor back in place rather than dirtying it: a
  // rebuild would hand the command processor a new descriptor and cost the
  // encoder restart this whole path exists to avoid, and an encoder already
  // created from it has its own copy of the load actions.
  SetCachedRenderPassLoadActions(pending_draw_pass_load_dontcare_mask_,
                                 MTL::LoadActionLoad);

  for (auto& transfers : pending_draw_pass_transfers_) {
    transfers.clear();
  }
  pending_draw_pass_render_targets_.fill(nullptr);
  pending_draw_pass_transfer_mask_ = 0;
  pending_draw_pass_full_overwrite_mask_ = 0;
  pending_draw_pass_load_dontcare_mask_ = 0;
}

bool MetalRenderTargetCache::BuildTransferRectanglePlans(
    RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
    const Transfer::Rectangle* cutout, bool require_all_rectangles,
    std::vector<TransferRectanglePlan>& transfer_rectangles_out) const {
  transfer_rectangles_out.clear();
  transfer_rectangles_out.reserve(transfers.size());
  for (uint32_t transfer_index = 0; transfer_index < transfers.size();
       ++transfer_index) {
    const Transfer& transfer = transfers[transfer_index];
    TransferRectanglePlan plan;
    plan.transfer_index = transfer_index;
    plan.rectangle_count = transfer.GetRectangles(
        dest_key, dest_key.Is64bpp(), plan.rectangles.data(), cutout);
    if (!plan.rectangle_count) {
      if (require_all_rectangles) {
        transfer_rectangles_out.clear();
        return false;
      }
      continue;
    }
    transfer_rectangles_out.push_back(plan);
  }
  return true;
}

bool MetalRenderTargetCache::CanQueueDrawPassTransfers(
    uint32_t render_target_index, RenderTarget* const* render_targets,
    const std::vector<Transfer>& transfers) const {
  if (!render_targets || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }
  auto* dest_metal_rt =
      static_cast<MetalRenderTarget*>(render_targets[render_target_index]);
  if (!dest_metal_rt) {
    return false;
  }
  RenderTargetKey dest_key = dest_metal_rt->key();
  if (dest_key.is_depth != (render_target_index == 0)) {
    return false;
  }

  // The transfer draws have to write through the very texture the pass binds.
  MTL::Texture* dest_draw_texture = dest_metal_rt->draw_texture();
  bool dest_is_uint = false;
  if (dest_key.is_depth) {
    if (!dest_draw_texture || dest_draw_texture != dest_metal_rt->texture() ||
        dest_draw_texture->pixelFormat() !=
            GetDepthPixelFormat(dest_key.GetDepthFormat())) {
      return false;
    }
  } else {
    MTL::PixelFormat transfer_format = GetColorOwnershipTransferPixelFormat(
        dest_key.GetColorFormat(), &dest_is_uint);
    if (dest_is_uint || !dest_draw_texture ||
        dest_draw_texture != dest_metal_rt->transfer_texture() ||
        GetColorDrawPixelFormat(dest_key.GetColorFormat()) != transfer_format ||
        dest_draw_texture->pixelFormat() != transfer_format) {
      return false;
    }
  }

  // Sampling a texture the same pass has bound as an attachment is undefined,
  // so anything the draw pass will hold has to stay out of the sources.
  auto is_active_draw_pass_texture = [&](const MetalRenderTarget* rt,
                                         MTL::Texture* texture) -> bool {
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      auto* active_rt = static_cast<MetalRenderTarget*>(render_targets[i]);
      if (!active_rt) {
        continue;
      }
      MTL::Texture* active_draw_texture = active_rt->draw_texture();
      if (rt == active_rt || texture == active_draw_texture ||
          (rt && rt->draw_texture() == active_draw_texture)) {
        return true;
      }
    }
    return false;
  };

  for (const Transfer& transfer : transfers) {
    if (!transfer.source) {
      return false;
    }
    auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
    if (source_rt == dest_metal_rt) {
      return false;
    }
    RenderTargetKey source_key = source_rt->key();
    if (transfer.host_depth_source) {
      if (!dest_key.is_depth) {
        return false;
      }
      auto* host_depth_rt =
          static_cast<MetalRenderTarget*>(transfer.host_depth_source);
      // A host depth source that is the destination itself goes through the
      // EDRAM round trip, which needs a compute pass of its own.
      if (!host_depth_rt || host_depth_rt == dest_metal_rt) {
        return false;
      }
      RenderTargetKey host_depth_key = host_depth_rt->key();
      MTL::Texture* host_depth_texture = host_depth_rt->texture();
      if (!host_depth_key.is_depth || !host_depth_texture ||
          host_depth_texture->pixelFormat() !=
              GetDepthPixelFormat(host_depth_key.GetDepthFormat()) ||
          is_active_draw_pass_texture(host_depth_rt, host_depth_texture)) {
        return false;
      }
    }

    MTL::Texture* source_texture = source_key.is_depth
                                       ? source_rt->texture()
                                       : source_rt->transfer_texture();
    if (!source_texture) {
      return false;
    }
    MTL::PixelFormat source_format =
        source_key.is_depth ? GetDepthPixelFormat(source_key.GetDepthFormat())
                            : GetColorOwnershipTransferPixelFormat(
                                  source_key.GetColorFormat(), nullptr);
    if (source_texture->pixelFormat() != source_format ||
        is_active_draw_pass_texture(source_rt, source_texture)) {
      return false;
    }
  }

  std::vector<TransferRectanglePlan> transfer_rectangles;
  return BuildTransferRectanglePlans(dest_key, transfers, nullptr, true,
                                     transfer_rectangles);
}

bool MetalRenderTargetCache::PendingDrawPassTransfersFullyOverwriteTarget(
    uint32_t render_target_index, RenderTarget* render_target,
    const std::vector<Transfer>& transfers) const {
  if (!render_target || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }

  auto* dest_metal_rt = static_cast<MetalRenderTarget*>(render_target);
  RenderTargetKey dest_key = dest_metal_rt->key();
  MTL::Texture* dest_texture = dest_metal_rt->draw_texture();
  if (!dest_texture) {
    return false;
  }
  uint32_t dest_width = uint32_t(dest_texture->width());
  uint32_t dest_height = uint32_t(dest_texture->height());
  if (!dest_width || !dest_height) {
    return false;
  }

  std::vector<TransferRectanglePlan> transfer_rectangles;
  if (!BuildTransferRectanglePlans(dest_key, transfers, nullptr, true,
                                   transfer_rectangles) ||
      transfer_rectangles.size() != transfers.size()) {
    return false;
  }
  for (const TransferRectanglePlan& transfer_plan : transfer_rectangles) {
    if (transfer_plan.rectangle_count != 1) {
      return false;
    }
    const Transfer::Rectangle& rect = transfer_plan.rectangles[0];
    if (rect.x_pixels || rect.y_pixels ||
        rect.width_pixels * draw_resolution_scale_x() < dest_width ||
        rect.height_pixels * draw_resolution_scale_y() < dest_height) {
      return false;
    }
  }
  return true;
}

bool MetalRenderTargetCache::GetActiveTransferAttachmentFormats(
    MTL::RenderPassDescriptor* pass_descriptor,
    TransferAttachmentFormats& attachment_formats_out) const {
  attachment_formats_out = TransferAttachmentFormats();
  if (!pass_descriptor) {
    return false;
  }

  if (auto* color_attachments = pass_descriptor->colorAttachments()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      auto* color_attachment = color_attachments->object(i);
      MTL::Texture* texture =
          color_attachment ? color_attachment->texture() : nullptr;
      if (texture) {
        attachment_formats_out.color_attachment_formats[i] =
            texture->pixelFormat();
      }
    }
  }
  if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
    if (MTL::Texture* texture = depth_attachment->texture()) {
      attachment_formats_out.depth_attachment_format = texture->pixelFormat();
    }
  }
  if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
    if (MTL::Texture* texture = stencil_attachment->texture()) {
      attachment_formats_out.stencil_attachment_format = texture->pixelFormat();
    }
  }
  return true;
}

bool MetalRenderTargetCache::GetCurrentTransferAttachmentFormats(
    TransferAttachmentFormats& attachment_formats_out) const {
  attachment_formats_out = TransferAttachmentFormats();

  bool has_color_attachment = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MTL::Texture* texture = current_color_targets_[i]
                                ? current_color_targets_[i]->draw_texture()
                                : nullptr;
    if (!texture) {
      continue;
    }
    attachment_formats_out.color_attachment_formats[i] = texture->pixelFormat();
    has_color_attachment = true;
  }
  // GetRenderPassDescriptor attaches a dummy 8_8_8_8 target at slot 0 when the
  // guest binds no color target.
  if (!has_color_attachment) {
    attachment_formats_out.color_attachment_formats[0] =
        GetColorDrawPixelFormat(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  }

  MTL::Texture* depth_texture =
      current_depth_target_ ? current_depth_target_->draw_texture() : nullptr;
  if (depth_texture) {
    MTL::PixelFormat depth_pixel_format = depth_texture->pixelFormat();
    attachment_formats_out.depth_attachment_format = depth_pixel_format;
    if (depth_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatX32_Stencil8) {
      attachment_formats_out.stencil_attachment_format = depth_pixel_format;
    }
  }
  return true;
}

bool MetalRenderTargetCache::PreflightPendingDrawPassTransfers(
    const TransferAttachmentFormats& attachment_formats) {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }

  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
      continue;
    }
    auto* dest_metal_rt =
        static_cast<MetalRenderTarget*>(pending_draw_pass_render_targets_[i]);
    if (!dest_metal_rt || pending_draw_pass_transfers_[i].empty()) {
      return false;
    }

    RenderTargetKey dest_key = dest_metal_rt->key();
    uint32_t dest_sample_count = MsaaSamplesToCount(dest_key.msaa_samples);
    bool dest_is_uint = false;
    uint32_t color_attachment_index = 0;
    MTL::PixelFormat dest_format = MTL::PixelFormatInvalid;
    if (dest_key.is_depth) {
      dest_format = GetDepthPixelFormat(dest_key.GetDepthFormat());
      // Every depth destination also runs the stencil clear and the stencil
      // draws, which may fall back to the per-bit states at encode time.
      if (i != 0 || attachment_formats.depth_attachment_format != dest_format ||
          !GetTransferDepthStencilState(true) ||
          !GetTransferStencilClearState() ||
          !GetOrCreateTransferClearPipeline(
              dest_format, false, true, dest_sample_count, 0,
              &attachment_formats.color_attachment_formats,
              attachment_formats.depth_attachment_format,
              attachment_formats.stencil_attachment_format)) {
        return false;
      }
      for (uint32_t bit = 0; bit < 8; ++bit) {
        if (!GetTransferStencilBitState(bit)) {
          return false;
        }
      }
      if ((dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
           dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) &&
          attachment_formats.stencil_attachment_format != dest_format) {
        return false;
      }
    } else {
      if (!i) {
        return false;
      }
      color_attachment_index = i - 1;
      dest_format = GetColorOwnershipTransferPixelFormat(
          dest_key.GetColorFormat(), &dest_is_uint);
      if (dest_is_uint ||
          attachment_formats.color_attachment_formats[color_attachment_index] !=
              dest_format ||
          !GetTransferNoDepthStencilState()) {
        return false;
      }
    }

    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
      if (!source_rt) {
        return false;
      }
      RenderTargetKey source_key = source_rt->key();
      if (source_key.is_depth && !GetStencilTextureView(source_rt)) {
        return false;
      }
      RenderTargetKey host_depth_key;
      bool has_host_depth = transfer.host_depth_source != nullptr;
      if (has_host_depth) {
        host_depth_key =
            static_cast<MetalRenderTarget*>(transfer.host_depth_source)->key();
      }
      // CanQueueDrawPassTransfers rejected a host depth source that is the
      // destination, so the copy-through-EDRAM mode never appears here.
      // Mirror the encode side: with the stencil folded into the depth draw
      // there is no stencil-bit pass to build pipelines for.
      bool native_stencil_output =
          dest_key.is_depth && UseNativeStencilOutputInTransfers();
      uint32_t stencil_bit_passes =
          (dest_key.is_depth && !native_stencil_output) ? 1u : 0u;
      for (uint32_t stencil_bit = 0; stencil_bit <= stencil_bit_passes;
           ++stencil_bit) {
        EdramTransferShaderKey shader_key = GetTransferShaderKey(
            source_key, dest_key,
            (has_host_depth && !stencil_bit) ? &host_depth_key : nullptr, false,
            stencil_bit != 0, color_attachment_index);
        if (!GetOrCreateTransferPipelines(
                shader_key, dest_format, dest_is_uint,
                native_stencil_output && !stencil_bit, color_attachment_index,
                &attachment_formats.color_attachment_formats,
                attachment_formats.depth_attachment_format,
                attachment_formats.stencil_attachment_format)) {
          return false;
        }
      }
    }
  }

  return true;
}

bool MetalRenderTargetCache::PreflightPendingDrawPassTransfers(
    MTL::RenderPassDescriptor* pass_descriptor) {
  TransferAttachmentFormats attachment_formats;
  if (!GetActiveTransferAttachmentFormats(pass_descriptor,
                                          attachment_formats)) {
    return false;
  }
  return PreflightPendingDrawPassTransfers(attachment_formats);
}

bool MetalRenderTargetCache::EncodePendingDrawPassTransfers(
    MTL::RenderCommandEncoder* encoder,
    MTL::RenderPassDescriptor* pass_descriptor,
    DrawPassTransferEncoderMutationMask* mutations_out) {
  if (mutations_out) {
    *mutations_out = kDrawPassTransferEncoderMutationNone;
  }
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!encoder || !PreflightPendingDrawPassTransfers(pass_descriptor)) {
    return false;
  }
  if (!PerformTransfersAndResolveClears(
          1 + xenos::kMaxColorRenderTargets,
          pending_draw_pass_render_targets_.data(),
          pending_draw_pass_transfers_.data(), nullptr, nullptr, nullptr,
          encoder, pass_descriptor, mutations_out)) {
    return false;
  }
  ClearPendingDrawPassTransfers();
  return true;
}

bool MetalRenderTargetCache::FlushPendingDrawPassTransfers() {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!PerformTransfersAndResolveClears(
          1 + xenos::kMaxColorRenderTargets,
          pending_draw_pass_render_targets_.data(),
          pending_draw_pass_transfers_.data())) {
    return false;
  }
  ClearPendingDrawPassTransfers();
  return true;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetWidth() const {
  // Metal maximum texture dimension
  return 16384;
}

uint32_t MetalRenderTargetCache::GetMaxRenderTargetHeight() const {
  // Metal maximum texture dimension
  return 16384;
}

bool MetalRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

RenderTargetCache::RenderTarget* MetalRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  SCOPE_profile_cpu_f("gpu");
  // Calculate dimensions
  uint32_t width = key.GetWidth();
  uint32_t height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples);

  // Apply resolution scaling
  width *= draw_resolution_scale_x();
  height *= draw_resolution_scale_y();

  // Create Metal render target
  auto* render_target = new MetalRenderTarget(key);

  // Create the texture based on format
  MTL::Texture* texture = nullptr;
  uint32_t samples = 1 << uint32_t(key.msaa_samples);

  if (key.is_depth) {
    texture = CreateDepthTexture(width, height, key.GetDepthFormat(), samples);
  } else {
    texture = CreateColorTexture(width, height, key.GetColorFormat(), samples);
  }

  if (!texture) {
    delete render_target;
    return nullptr;
  }

  render_target->SetTexture(texture);
  if (!key.is_depth) {
    MTL::PixelFormat resource_format =
        GetColorResourcePixelFormat(key.GetColorFormat());
    MTL::PixelFormat draw_format =
        GetColorDrawPixelFormat(key.GetColorFormat());
    MTL::PixelFormat transfer_format =
        GetColorOwnershipTransferPixelFormat(key.GetColorFormat(), nullptr);
    if (draw_format != resource_format) {
      MTL::Texture* draw_view = texture->newTextureView(draw_format);
      RecordRenderTargetViewCreated();
      render_target->SetDrawTexture(draw_view);
    }
    if (transfer_format != resource_format) {
      MTL::Texture* transfer_view = texture->newTextureView(transfer_format);
      RecordRenderTargetViewCreated();
      render_target->SetTransferTexture(transfer_view);
    }
  }

  // NOTE: Unlike the previous implementation, we do NOT load EDRAM data here.
  // This matches D3D12's approach where:
  // 1. CreateRenderTarget creates an empty texture
  // 2. Data transfer happens via ownership transfers in
  // PerformTransfersAndResolveClears
  // 3. The EDRAM buffer is only used as scratch space for resolves
  //
  // The ownership transfer system (called from Update()) handles copying data
  // between render target textures when EDRAM regions are aliased between
  // different RT configurations.

  // Store in our map for later retrieval
  render_target_map_[key.key] = render_target;

  return render_target;
}

bool MetalRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  // Metal uses different depth encoding than Xbox 360
  // D24S8 on Xbox 360 vs D32Float_S8 on Metal
  return format == xenos::DepthRenderTargetFormat::kD24S8 ||
         format == xenos::DepthRenderTargetFormat::kD24FS8;
}

void MetalRenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (!snapshot) {
    return;
  }

  if (IsDrawResolutionScaled()) {
    return;
  }

  RenderTarget* full_edram_rt =
      PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
          xenos::ColorRenderTargetFormat::k_32_FLOAT);
  if (!full_edram_rt) {
    return;
  }

  MetalRenderTarget* metal_rt = static_cast<MetalRenderTarget*>(full_edram_rt);
  MTL::Texture* texture = metal_rt->texture();
  if (!texture) {
    return;
  }

  constexpr uint32_t kPitchTilesAt32bpp = 16;
  constexpr uint32_t kWidth =
      kPitchTilesAt32bpp * xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileRows = xenos::kEdramTileCount / kPitchTilesAt32bpp;
  constexpr uint32_t kHeight = kTileRows * xenos::kEdramTileHeightSamples;

  size_t staging_size = size_t(kWidth) * size_t(kHeight) * sizeof(uint32_t);
  MTL::Buffer* staging =
      device_->newBuffer(staging_size, MTL::ResourceStorageModeShared);
  if (!staging) {
    return;
  }

  auto* dst_base = static_cast<uint8_t*>(staging->contents());
  const uint8_t* src = static_cast<const uint8_t*>(snapshot);
  uint32_t bytes_per_row = kWidth * sizeof(uint32_t);

  for (uint32_t y_tile = 0; y_tile < kTileRows; ++y_tile) {
    for (uint32_t x_tile = 0; x_tile < kPitchTilesAt32bpp; ++x_tile) {
      uint32_t tile_index = y_tile * kPitchTilesAt32bpp + x_tile;
      const uint8_t* tile_src =
          src + tile_index * xenos::kEdramTileWidthSamples *
                    xenos::kEdramTileHeightSamples * sizeof(uint32_t);

      for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples;
           ++sample_row) {
        uint32_t dst_y = y_tile * xenos::kEdramTileHeightSamples + sample_row;
        uint32_t dst_x = x_tile * xenos::kEdramTileWidthSamples;

        uint8_t* dst_row =
            dst_base + dst_y * bytes_per_row + dst_x * sizeof(uint32_t);
        const uint8_t* src_row = tile_src + sample_row *
                                                xenos::kEdramTileWidthSamples *
                                                sizeof(uint32_t);

        std::memcpy(dst_row, src_row,
                    xenos::kEdramTileWidthSamples * sizeof(uint32_t));
      }
    }
  }

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    staging->release();
    return;
  }

  MTL::CommandBuffer* cmd = command_processor_.CreateAccountedCommandBuffer(
      MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
  if (!cmd) {
    staging->release();
    return;
  }

  MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
  if (!blit) {
    command_processor_.DiscardAccountedCommandBuffer(
        cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
    staging->release();
    return;
  }

  blit->copyFromBuffer(staging, 0, bytes_per_row, 0,
                       MTL::Size::Make(kWidth, kHeight, 1), texture, 0, 0,
                       MTL::Origin::Make(0, 0, 0));
  blit->endEncoding();
  cmd->commit();
  cmd->waitUntilCompleted();
  // cmd is autoreleased from commandBuffer() - do not release
  staging->release();
  if (metal_rt->needs_initial_clear()) {
    metal_rt->SetNeedsInitialClear(false);
    render_pass_descriptor_dirty_ = true;
  }

  // Seed edram_buffer_ with the restored full-EDRAM render target contents
  // so subsequent DumpRenderTargets and resolve passes see the same initial
  // EDRAM state as D3D12/Vulkan.
  DumpRenderTargets(0, kPitchTilesAt32bpp, kTileRows, kPitchTilesAt32bpp);
}

MTL::Texture* MetalRenderTargetCache::CreateColorTexture(
    uint32_t width, uint32_t height, xenos::ColorRenderTargetFormat format,
    uint32_t samples, bool transient_render_target_only) {
  MTL::PixelFormat resource_format = GetColorResourcePixelFormat(format);
  MTL::PixelFormat draw_format = GetColorDrawPixelFormat(format);
  MTL::PixelFormat transfer_format =
      GetColorOwnershipTransferPixelFormat(format, nullptr);
  bool needs_pixel_format_view =
      draw_format != resource_format || transfer_format != resource_format;

  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  desc->setPixelFormat(resource_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage = MTL::TextureUsageRenderTarget;
  if (!transient_render_target_only) {
    usage |= MTL::TextureUsageShaderRead;
  }
  if (needs_pixel_format_view) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);

  MTL::Texture* texture = nullptr;
  bool can_use_memoryless = false;
#if XE_PLATFORM_IOS
  can_use_memoryless = transient_render_target_only && !needs_pixel_format_view;
#endif
  if (can_use_memoryless) {
    // Dummy fallback color targets are transient (load/store don't care) and
    // never sampled - memoryless is optimal on iOS TBDR.
    desc->setStorageMode(MTL::StorageModeMemoryless);
    texture = device_->newTexture(desc);
  }
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    texture = device_->newTexture(desc);
  }
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateDummyColorTexture(
    uint32_t width, uint32_t height, uint32_t samples) {
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);
  // Nothing reads this and the write mask is empty, so the narrowest renderable
  // format will do - it still costs tile memory on every depth-only pass.
  desc->setPixelFormat(MTL::PixelFormatR8Unorm);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  desc->setUsage(MTL::TextureUsageRenderTarget);

  MTL::Texture* texture = nullptr;
#if XE_PLATFORM_IOS
  desc->setStorageMode(MTL::StorageModeMemoryless);
  texture = device_->newTexture(desc);
#endif
  if (!texture) {
    desc->setStorageMode(MTL::StorageModePrivate);
    texture = device_->newTexture(desc);
  }
  desc->release();
  return texture;
}

MTL::Texture* MetalRenderTargetCache::CreateDepthTexture(
    uint32_t width, uint32_t height, xenos::DepthRenderTargetFormat format,
    uint32_t samples) {
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(width);
  desc->setHeight(height ? height : 720);  // Default height if not specified
  MTL::PixelFormat pixel_format = GetDepthPixelFormat(format);
  desc->setPixelFormat(pixel_format);
  desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample
                                   : MTL::TextureType2D);
  desc->setSampleCount(samples);
  MTL::TextureUsage usage =
      MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
  if (pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
      pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);
  desc->setStorageMode(MTL::StorageModePrivate);

  MTL::Texture* texture = device_->newTexture(desc);
  desc->release();
  // Initial clear is handled on first bind via load actions; avoid
  // synchronous clears here to keep the host RT path fast.
  return texture;
}

MTL::PixelFormat MetalRenderTargetCache::GetColorResourcePixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // Updated to suport unorm16 for gamma render targets.
      return gamma_render_target_as_unorm16_ ? MTL::PixelFormatRGBA16Unorm
                                             : MTL::PixelFormatRGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      // Match D3D12 behavior: store as RGBA16F and pack to float10 on dump.
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      XELOGE("MetalRenderTargetCache: Unsupported color format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatRGBA8Unorm;
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorDrawPixelFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    default:
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetColorOwnershipTransferPixelFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Uint;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Uint;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Uint;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Uint;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      // Ownership transfers must use a linear resource view to avoid
      // implicit sRGB conversion for gamma render targets.
      return GetColorResourcePixelFormat(format);
  }
}

MTL::PixelFormat MetalRenderTargetCache::GetDepthPixelFormat(
    xenos::DepthRenderTargetFormat format) const {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
    case xenos::DepthRenderTargetFormat::kD24FS8:
      // Metal doesn't have D24S8, use D32Float_S8
      return MTL::PixelFormatDepth32Float_Stencil8;
    default:
      XELOGE("MetalRenderTargetCache: Unsupported depth format {}",
             static_cast<uint32_t>(format));
      return MTL::PixelFormatDepth32Float_Stencil8;
  }
}

MTL::Texture* MetalRenderTargetCache::GetStencilTextureView(
    MetalRenderTarget* render_target) {
  if (!render_target) {
    return nullptr;
  }
  if (render_target->stencil_view()) {
    return render_target->stencil_view();
  }
  RenderTargetKey key = render_target->key();
  if (!key.is_depth) {
    return nullptr;
  }
  MTL::Texture* depth_texture = render_target->texture();
  if (!depth_texture) {
    return nullptr;
  }
  MTL::Texture* view =
      depth_texture->newTextureView(MTL::PixelFormatX32_Stencil8);
  if (view) {
    render_target->SetStencilView(view);
  }
  return view;
}

MTL::RenderPassDescriptor* MetalRenderTargetCache::GetRenderPassDescriptor(
    uint32_t expected_sample_count, bool render_encoder_pending) {
  if (!render_pass_descriptor_dirty_ && cached_render_pass_descriptor_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count) {
    // Queuing transfers deliberately does not dirty the descriptor, so their
    // load actions are the one thing this path still has to bring up to date.
    if (render_encoder_pending) {
      ApplyPendingDrawPassLoadActions();
    }
    return cached_render_pass_descriptor_;
  }
  if (cached_render_pass_descriptor_sample_count_ != expected_sample_count) {
    render_pass_descriptor_dirty_ = true;
  }

  // Release old descriptor
  if (cached_render_pass_descriptor_) {
    cached_render_pass_descriptor_->release();
    cached_render_pass_descriptor_ = nullptr;
  }

  // Create new descriptor
  cached_render_pass_descriptor_ =
      MTL::RenderPassDescriptor::renderPassDescriptor();
  if (!cached_render_pass_descriptor_) {
    XELOGE("MetalRenderTargetCache: Failed to create render pass descriptor");
    return nullptr;
  }
  cached_render_pass_descriptor_->retain();
  cached_render_pass_descriptor_sample_count_ = expected_sample_count;
  cached_render_pass_descriptor_pending_clears_.fill(nullptr);

  // Queued transfers that rewrite a destination in full make loading its old
  // contents into tile memory pointless, but only for the pass that actually
  // encodes them - ClearPendingDrawPassTransfers restores this if it doesn't.
  pending_draw_pass_load_dontcare_mask_ = GetPendingDrawPassLoadDontCareMask();
  auto pending_load_dontcare = [this](uint32_t pending_index) {
    return (pending_draw_pass_load_dontcare_mask_ &
            (uint32_t(1) << pending_index)) != 0;
  };

  bool has_any_render_target = false;
  bool has_any_color_target = false;
  uint32_t coverage_width = 0;
  uint32_t coverage_height = 0;
  uint32_t coverage_samples = std::max(1u, expected_sample_count);

  // Bind the actual render targets retrieved from base class in Update()

  // Bind depth target if present
  if (current_depth_target_ && current_depth_target_->texture()) {
    auto* depth_attachment = cached_render_pass_descriptor_->depthAttachment();
    depth_attachment->setTexture(current_depth_target_->draw_texture());

    // Clear on first bind to avoid synchronous clears at creation.
    uint32_t depth_key = current_depth_target_->key().key;
    bool depth_needs_clear = current_depth_target_->needs_initial_clear();
    bool depth_load_dontcare = pending_load_dontcare(0);
    if (depth_needs_clear) {
      depth_attachment->setLoadAction(MTL::LoadActionClear);
      depth_attachment->setClearDepth(1.0);
      cached_render_pass_descriptor_pending_clears_[0] = current_depth_target_;
    } else {
      depth_attachment->setLoadAction(
          depth_load_dontcare ? MTL::LoadActionDontCare : MTL::LoadActionLoad);
    }
    depth_attachment->setStoreAction(MTL::StoreActionStore);

    // If the depth texture includes stencil, bind the same texture to the
    // stencil attachment too (Metal requires explicit stencil attachment
    // binding to match pipeline state).
    MTL::PixelFormat depth_pixel_format =
        current_depth_target_->draw_texture()->pixelFormat();
    if (depth_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_pixel_format == MTL::PixelFormatX32_Stencil8) {
      auto* stencil_attachment =
          cached_render_pass_descriptor_->stencilAttachment();
      stencil_attachment->setTexture(current_depth_target_->draw_texture());
      if (depth_needs_clear) {
        stencil_attachment->setLoadAction(MTL::LoadActionClear);
        stencil_attachment->setClearStencil(0);
      } else {
        // The queued depth transfers clear and rewrite stencil over the same
        // rectangles they cover.
        stencil_attachment->setLoadAction(depth_load_dontcare
                                              ? MTL::LoadActionDontCare
                                              : MTL::LoadActionLoad);
      }
      stencil_attachment->setStoreAction(MTL::StoreActionStore);
    }

    has_any_render_target = true;

    // Track this as a real render target for capture
    last_real_depth_target_ = current_depth_target_;

    if (!coverage_width && current_depth_target_->draw_texture()) {
      coverage_width =
          static_cast<uint32_t>(current_depth_target_->draw_texture()->width());
      coverage_height = static_cast<uint32_t>(
          current_depth_target_->draw_texture()->height());
      if (current_depth_target_->draw_texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                current_depth_target_->draw_texture()->sampleCount()));
      }
    }
  }

  // Bind color targets
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_color_targets_[i] && current_color_targets_[i]->texture()) {
      auto* color_attachment =
          cached_render_pass_descriptor_->colorAttachments()->object(i);
      color_attachment->setTexture(current_color_targets_[i]->draw_texture());

      // Clear on first bind to avoid synchronous clears at creation.
      bool color_needs_clear = current_color_targets_[i]->needs_initial_clear();
      if (color_needs_clear) {
        color_attachment->setLoadAction(MTL::LoadActionClear);
        color_attachment->setClearColor(
            MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));
        cached_render_pass_descriptor_pending_clears_[1 + i] =
            current_color_targets_[i];
      } else {
        color_attachment->setLoadAction(pending_load_dontcare(i + 1)
                                            ? MTL::LoadActionDontCare
                                            : MTL::LoadActionLoad);
      }
      color_attachment->setStoreAction(MTL::StoreActionStore);

      has_any_render_target = true;
      has_any_color_target = true;

      // Track this as a real render target for capture
      last_real_color_targets_[i] = current_color_targets_[i];

      if (!coverage_width) {
        coverage_width = static_cast<uint32_t>(
            current_color_targets_[i]->draw_texture()->width());
        coverage_height = static_cast<uint32_t>(
            current_color_targets_[i]->draw_texture()->height());
        if (current_color_targets_[i]->draw_texture()->sampleCount() > 0) {
          coverage_samples = std::max<uint32_t>(
              coverage_samples,
              static_cast<uint32_t>(
                  current_color_targets_[i]->draw_texture()->sampleCount()));
        }
      }
    }
  }

  // If no color render targets are bound, attach a dummy color target so Metal
  // has at least one color attachment. This mirrors the D3D12/Vulkan behavior
  // where an RTV is always bound when drawing, and also keeps pipeline state
  // validation happy for depth-only passes.
  if (!has_any_color_target) {
    uint32_t samples = std::max(1u, expected_sample_count);

    uint32_t width = 1280;
    uint32_t height = 720;
    if (current_depth_target_ && current_depth_target_->texture()) {
      width = static_cast<uint32_t>(current_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(current_depth_target_->texture()->height());
      if (current_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         current_depth_target_->texture()->sampleCount()));
      }
    } else if (last_real_color_targets_[0] &&
               last_real_color_targets_[0]->texture()) {
      width = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->width());
      height = static_cast<uint32_t>(
          last_real_color_targets_[0]->texture()->height());
    } else if (last_real_depth_target_ && last_real_depth_target_->texture()) {
      width =
          static_cast<uint32_t>(last_real_depth_target_->texture()->width());
      height =
          static_cast<uint32_t>(last_real_depth_target_->texture()->height());
      if (last_real_depth_target_->texture()->sampleCount() > 0) {
        samples = std::max<uint32_t>(
            samples, static_cast<uint32_t>(
                         last_real_depth_target_->texture()->sampleCount()));
      }
    }

    uint32_t dummy_sample_count =
        samples >= 4u ? 4u : (samples == 2u ? 2u : 1u);
    // Cache dummy color targets by shape only so depth-only passes with
    // changing EDRAM bases can reuse the same transient attachment.
    uint64_t dummy_key = uint64_t(width & 0xFFFFu) |
                         (uint64_t(height & 0xFFFFu) << 16) |
                         (uint64_t(dummy_sample_count & 0xFFu) << 32);
    auto evict_oldest_dummy_target = [&](uint64_t keep_key) -> bool {
      uint64_t oldest_key = 0;
      uint64_t oldest_frame = frame_id_;
      bool found = false;
      for (const auto& it : dummy_color_targets_) {
        if (it.first == keep_key) {
          continue;
        }
        if (!found || it.second.last_used_frame < oldest_frame) {
          oldest_frame = it.second.last_used_frame;
          oldest_key = it.first;
          found = true;
        }
      }
      if (found) {
        dummy_color_targets_.erase(oldest_key);
      }
      return found;
    };

    auto& entry = dummy_color_targets_[dummy_key];
    if (!entry.target || !entry.target->texture()) {
      RenderTargetKey dummy_rt_key;
      dummy_rt_key.key = 0;
      dummy_rt_key.is_depth = 0;
      dummy_rt_key.msaa_samples =
          dummy_sample_count >= 4u   ? xenos::MsaaSamples::k4X
          : dummy_sample_count == 2u ? xenos::MsaaSamples::k2X
                                     : xenos::MsaaSamples::k1X;
      entry.target = std::make_unique<MetalRenderTarget>(dummy_rt_key);
      entry.last_cleared_frame = frame_id_ - 1;
      MTL::Texture* tex =
          CreateDummyColorTexture(width, height, dummy_sample_count);
      while (!tex && dummy_color_targets_.size() > 1 &&
             evict_oldest_dummy_target(dummy_key)) {
        tex = CreateDummyColorTexture(width, height, dummy_sample_count);
      }
      entry.target->SetTexture(tex);
    }

    entry.last_used_frame = frame_id_;
    dummy_color_target_ = entry.target.get();

    // Keep this cache small - dummy targets are transient fallback attachments.
#if XE_PLATFORM_IOS
    constexpr size_t kMaxDummyColorTargets = 4;
#else
    constexpr size_t kMaxDummyColorTargets = 8;
#endif
    while (dummy_color_targets_.size() > kMaxDummyColorTargets) {
      if (!evict_oldest_dummy_target(dummy_key)) {
        break;
      }
    }

    auto* color_attachment =
        cached_render_pass_descriptor_->colorAttachments()->object(0);
    color_attachment->setTexture(dummy_color_target_->draw_texture());
    color_attachment->setLoadAction(MTL::LoadActionDontCare);
    color_attachment->setStoreAction(MTL::StoreActionDontCare);

    has_any_render_target = true;
    if (!coverage_width && dummy_color_target_->draw_texture()) {
      coverage_width =
          static_cast<uint32_t>(dummy_color_target_->draw_texture()->width());
      coverage_height =
          static_cast<uint32_t>(dummy_color_target_->draw_texture()->height());
      if (dummy_color_target_->draw_texture()->sampleCount() > 0) {
        coverage_samples = std::max<uint32_t>(
            coverage_samples,
            static_cast<uint32_t>(
                dummy_color_target_->draw_texture()->sampleCount()));
      }
    }
  }

  render_pass_descriptor_dirty_ = false;
  return cached_render_pass_descriptor_;
}

void MetalRenderTargetCache::ConsumeRenderPassDescriptorClears(
    MTL::RenderPassDescriptor* pass_descriptor) {
  if (!pass_descriptor || pass_descriptor != cached_render_pass_descriptor_) {
    return;
  }
  render_pass_encoder_created_since_targets_changed_ = true;
  bool any_consumed = false;
  for (MetalRenderTarget*& render_target :
       cached_render_pass_descriptor_pending_clears_) {
    if (!render_target) {
      continue;
    }
    render_target->SetNeedsInitialClear(false);
    render_target = nullptr;
    any_consumed = true;
  }
  if (any_consumed) {
    // The cached descriptor still has clear load actions. Rebuild it before the
    // next encoder so the cleared contents are loaded rather than cleared
    // again.
    render_pass_descriptor_dirty_ = true;
  }
}

bool MetalRenderTargetCache::IsRenderPassDescriptorCompatible(
    MTL::RenderPassDescriptor* pass_descriptor,
    uint32_t expected_sample_count) const {
  if (pass_descriptor && pass_descriptor == cached_render_pass_descriptor_ &&
      !render_pass_descriptor_dirty_ &&
      cached_render_pass_descriptor_sample_count_ == expected_sample_count) {
    return true;
  }
  if (!pass_descriptor) {
    return false;
  }

  auto* depth_attachment = pass_descriptor->depthAttachment();
  auto* stencil_attachment = pass_descriptor->stencilAttachment();
  MTL::Texture* expected_depth =
      current_depth_target_ ? current_depth_target_->draw_texture() : nullptr;
  if (expected_depth) {
    if (!depth_attachment || depth_attachment->texture() != expected_depth) {
      return false;
    }
    MTL::PixelFormat depth_format = expected_depth->pixelFormat();
    bool expects_stencil =
        depth_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        depth_format == MTL::PixelFormatDepth24Unorm_Stencil8 ||
        depth_format == MTL::PixelFormatX32_Stencil8;
    if (expects_stencil) {
      if (!stencil_attachment ||
          stencil_attachment->texture() != expected_depth) {
        return false;
      }
    } else if (stencil_attachment && stencil_attachment->texture()) {
      return false;
    }
  } else if ((depth_attachment && depth_attachment->texture()) ||
             (stencil_attachment && stencil_attachment->texture())) {
    return false;
  }

  bool has_current_color_target = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (current_color_targets_[i] &&
        current_color_targets_[i]->draw_texture()) {
      has_current_color_target = true;
      break;
    }
  }
  auto* color_attachments = pass_descriptor->colorAttachments();
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MTL::Texture* actual_color = color_attachments->object(i)->texture();
    MTL::Texture* expected_color =
        current_color_targets_[i] ? current_color_targets_[i]->draw_texture()
                                  : nullptr;
    if (expected_color) {
      if (actual_color != expected_color) {
        return false;
      }
    } else if (actual_color && (has_current_color_target || i != 0)) {
      return false;
    }
  }

  if (has_current_color_target) {
    return true;
  }
  MTL::Texture* expected_dummy =
      dummy_color_target_ ? dummy_color_target_->draw_texture() : nullptr;
  return expected_dummy &&
         color_attachments->object(0)->texture() == expected_dummy;
}

void MetalRenderTargetCache::RecordRenderTargetViewCreated() {
  render_target_views_created_.fetch_add(1, std::memory_order_relaxed);
}

MetalRenderTargetCache::MetalRenderTarget*
MetalRenderTargetCache::GetColorRenderTarget(uint32_t index) const {
  if (index >= 4) {
    return nullptr;
  }
  return current_color_targets_[index];
}

MTL::Texture* MetalRenderTargetCache::GetColorTargetForDraw(
    uint32_t index) const {
  if (index >= 4 || !current_color_targets_[index]) {
    return nullptr;
  }
  return current_color_targets_[index]->draw_texture();
}

MTL::Texture* MetalRenderTargetCache::GetDepthTargetForDraw() const {
  if (!current_depth_target_) {
    return nullptr;
  }
  return current_depth_target_->draw_texture();
}

MTL::Texture* MetalRenderTargetCache::GetDummyColorTargetForDraw() const {
  if (dummy_color_target_ && dummy_color_target_->draw_texture()) {
    return dummy_color_target_->draw_texture();
  }
  return nullptr;
}

MTL::Texture* MetalRenderTargetCache::GetLastRealColorTarget(
    uint32_t index) const {
  if (index >= 4 || !last_real_color_targets_[index]) {
    return nullptr;
  }
  return last_real_color_targets_[index]->texture();
}

MTL::Texture* MetalRenderTargetCache::GetLastRealDepthTarget() const {
  if (!last_real_depth_target_) {
    return nullptr;
  }
  return last_real_depth_target_->texture();
}

MTL::Texture* MetalRenderTargetCache::GetRenderTargetTexture(
    RenderTargetKey key) const {
  auto it = render_target_map_.find(key.key);
  if (it == render_target_map_.end()) {
    return nullptr;
  }
  MetalRenderTarget* target = it->second;
  return target ? target->texture() : nullptr;
}

MTL::Texture* MetalRenderTargetCache::GetColorRenderTargetTexture(
    uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
    xenos::ColorRenderTargetFormat format) const {
  if (!pitch) {
    return nullptr;
  }
  RenderTargetKey key;
  key.base_tiles = base;
  uint32_t msaa_samples_x_log2 = uint32_t(samples >= xenos::MsaaSamples::k4X);
  key.pitch_tiles_at_32bpp =
      ((pitch << msaa_samples_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  key.msaa_samples = samples;
  key.is_depth = 0;
  xenos::ColorRenderTargetFormat resource_format =
      (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
       !gamma_render_target_as_unorm16_)
          ? xenos::ColorRenderTargetFormat::k_8_8_8_8
          : xenos::GetStorageColorFormat(format);
  key.resource_format = uint32_t(resource_format);
  return GetRenderTargetTexture(key);
}

void MetalRenderTargetCache::StoreTiledData(MTL::CommandBuffer* command_buffer,
                                            MTL::Texture* texture,
                                            uint32_t edram_base,
                                            uint32_t pitch_tiles,
                                            uint32_t height_tiles,
                                            bool is_depth) {
  MTL::Texture* source_texture = texture;
  MTL::Texture* temp_texture = nullptr;

  // Check if this is a depth/stencil texture
  bool is_depth_stencil_format =
      texture->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8 ||
      texture->pixelFormat() == MTL::PixelFormatDepth32Float ||
      texture->pixelFormat() == MTL::PixelFormatDepth16Unorm ||
      texture->pixelFormat() == MTL::PixelFormatDepth24Unorm_Stencil8 ||
      texture->pixelFormat() == MTL::PixelFormatX32_Stencil8;

  if (is_depth_stencil_format) {
    // Depth/stencil textures can't be sampled directly from a compute shader,
    // so storing them back to EDRAM would need a depth-read intermediate path.
    // Depth buffers are typically write-only during rendering, so skip.
    return;
  }

  // If texture is multisample, create a temporary non-multisample texture and
  // resolve to it first
  if (texture->textureType() == MTL::TextureType2DMultisample) {
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setWidth(texture->width());
    desc->setHeight(texture->height());
    desc->setPixelFormat(texture->pixelFormat());
    desc->setTextureType(MTL::TextureType2D);  // Regular 2D texture
    desc->setSampleCount(1);                   // Non-multisample
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);

    temp_texture = device_->newTexture(desc);
    desc->release();
    if (!temp_texture) {
      XELOGE(
          "MetalRenderTargetCache::StoreTiledData - Failed to create "
          "temporary "
          "texture");
      return;
    }

    // Resolve multisample texture to temporary texture
    MTL::RenderPassDescriptor* resolve_desc =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    if (resolve_desc) {
      auto* color_attachment = resolve_desc->colorAttachments()->object(0);
      color_attachment->setTexture(texture);              // Multisample source
      color_attachment->setResolveTexture(temp_texture);  // Resolved output
      color_attachment->setLoadAction(MTL::LoadActionLoad);
      color_attachment->setStoreAction(MTL::StoreActionMultisampleResolve);

      MTL::RenderCommandEncoder* render_encoder =
          command_buffer->renderCommandEncoder(resolve_desc);
      if (render_encoder) {
        render_encoder->endEncoding();
        // render_encoder is autoreleased - do not release
      }
    }

    source_texture = temp_texture;
  }

  // Create compute encoder
  MTL::ComputeCommandEncoder* encoder = command_buffer->computeCommandEncoder();
  if (!encoder) {
    if (temp_texture) {
      temp_texture->release();
    }
    return;
  }

  // Set compute pipeline
  encoder->setComputePipelineState(edram_store_pipeline_);

  // Bind input texture (either original or resolved)
  encoder->setTexture(source_texture, 0);

  // Bind EDRAM buffer
  encoder->setBuffer(edram_buffer_, 0, 0);
  encoder->useResource(source_texture, MTL::ResourceUsageRead);
  encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);

  // Create parameter buffers
  uint32_t params[2] = {edram_base, pitch_tiles};
  MTL::Buffer* param_buffer = device_->newBuffer(
      &params, sizeof(params), MTL::ResourceStorageModeShared);
  encoder->setBuffer(param_buffer, 0, 1);
  encoder->setBuffer(param_buffer, sizeof(uint32_t), 2);

  // Calculate thread group sizes
  MTL::Size threads_per_threadgroup = MTL::Size::Make(8, 8, 1);
  MTL::Size threadgroups = MTL::Size::Make(
      (source_texture->width() + 7) / 8, (source_texture->height() + 7) / 8, 1);

  // Dispatch compute
  encoder->dispatchThreadgroups(threadgroups, threads_per_threadgroup);
  encoder->endEncoding();
  // encoder is autoreleased - do not release

  if (temp_texture) {
    temp_texture->release();
  }

  param_buffer->release();
}

MTL::ComputePipelineState* MetalRenderTargetCache::GetOrCreateDumpPipeline(
    EdramDumpShaderKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  EdramDumpShaderOptions shader_options;
  // The emitter declares the EDRAM buffer with the pre-1.3 BufferBlock and
  // Uniform forms, so it has to be emitted as SPIR-V 1.0.
  shader_options.spirv_version = 0x00010000;
  shader_options.descriptor_set_dest = 0;
  shader_options.descriptor_set_source = 1;
  shader_options.resolution_scale_x = draw_resolution_scale_x();
  shader_options.resolution_scale_y = draw_resolution_scale_y();
  shader_options.msaa_2x_attachments_supported = msaa_2x_supported_;
  if (!key.is_depth) {
    GetColorOwnershipTransferPixelFormat(key.GetColorFormat(),
                                         &shader_options.source_is_uint);
  }
  shader_options.depth_float24_round = ::cvars::depth_float24_round;
  shader_options.depth_float24_convert_in_pixel_shader =
      ::cvars::depth_float24_convert_in_pixel_shader;

  MTL::ComputePipelineState* pipeline = nullptr;
  std::vector<uint32_t> spirv = BuildEdramDumpShaderSpirv(key, shader_options);
  if (spirv.empty()) {
    XELOGE("MetalRenderTargetCache: failed to emit the dump shader 0x{:08X}",
           key.key);
  } else {
    std::vector<uint8_t> dxil = SpirvToDxilCompiler::Translate(
        spirv.data(), spirv.size(), SpirvToDxilCompiler::Stage::kCompute);
    if (dxil.empty()) {
      XELOGE(
          "MetalRenderTargetCache: failed to translate the dump shader "
          "0x{:08X}",
          key.key);
    } else {
      MetalShaderConversionResult conversion =
          command_processor_.metal_shader_converter().ConvertInternalCompute(
              dxil);
      if (!conversion.success) {
        XELOGE(
            "MetalRenderTargetCache: failed to convert the dump shader "
            "0x{:08X}: {}",
            key.key, conversion.error_message);
      } else {
        pipeline = CreateComputePipelineFromEmbeddedLibrary(
            device_, conversion.metallib.data(), conversion.metallib.size(),
            "edram_dump", conversion.entry_point_name.c_str());
      }
    }
  }
  if (!pipeline) {
    XELOGE(
        "MetalRenderTargetCache: no dump pipeline for key=0x{:08X} "
        "(is_depth={}, format={}, msaa={})",
        key.key, uint32_t(key.is_depth), uint32_t(key.resource_format),
        uint32_t(key.msaa_samples));
  }
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool MetalRenderTargetCache::DirectResolveRenderTargets(
    const draw_util::ResolveInfo& resolve_info,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, MTL::CommandBuffer* command_buffer) {
  SCOPE_profile_cpu_f("gpu");
  auto* shared = command_processor_.shared_memory();
  MTL::Buffer* dest_buffer = shared ? shared->GetBuffer() : nullptr;
  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!dest_buffer || !queue) {
    return false;
  }

  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rectangles);
  if (rectangles.empty()) {
    return false;
  }

  // Every pipeline and source has to resolve before anything is encoded -
  // skipping a rectangle partway through would leave its part of the
  // destination stale, with nothing to say so.
  struct Source {
    MTL::ComputePipelineState* pipeline;
    MTL::Texture* texture;
    MTL::Texture* stencil_texture;
  };
  std::vector<Source> sources;
  sources.reserve(rectangles.size());
  for (const ResolveCopyDumpRectangle& rectangle : rectangles) {
    auto* rt = static_cast<MetalRenderTarget*>(rectangle.render_target);
    if (!rt) {
      return false;
    }
    RenderTargetKey rt_key = rt->key();
    EdramDumpShaderKey shader_key;
    shader_key.is_depth = rt_key.is_depth;
    shader_key.resource_format = rt_key.resource_format;
    shader_key.msaa_samples = rt_key.msaa_samples;
    shader_key.direct_resolve = 1;
    Source source;
    source.pipeline = GetOrCreateDumpPipeline(shader_key);
    // The source is the ownership transfer view, whose format the shader was
    // emitted against - an integer one for the formats where exact bits matter.
    source.stencil_texture = nullptr;
    if (rt_key.is_depth) {
      source.texture = rt->texture();
      source.stencil_texture = GetStencilTextureView(rt);
      if (!source.stencil_texture) {
        return false;
      }
    } else {
      bool source_is_uint = false;
      GetColorOwnershipTransferPixelFormat(rt_key.GetColorFormat(),
                                           &source_is_uint);
      source.texture = source_is_uint ? rt->transfer_texture() : rt->texture();
    }
    if (!source.pipeline || !source.texture) {
      return false;
    }
    sources.push_back(source);
  }

  // The GPU is about to overwrite this range, so any CPU-side data has to be
  // uploaded first, as it is for the resolve copy.
  if (!shared->RequestRange(resolve_info.copy_dest_extent_start,
                            resolve_info.copy_dest_extent_length)) {
    XELOGE(
        "MetalRenderTargetCache::DirectResolveRenderTargets: RequestRange "
        "failed for 0x{:08X} len {}",
        resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length);
    return false;
  }

  ScopedAutoreleasePool autorelease_pool;
  bool owns_command_buffer = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
    if (!cmd) {
      return false;
    }
    owns_command_buffer = true;
  }
  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    if (owns_command_buffer) {
      command_processor_.DiscardAccountedCommandBuffer(
          cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
    }
    return false;
  }

  const MetalShaderConverter& converter =
      command_processor_.metal_shader_converter();

  EdramDumpShaderPitches pitches;
  pitches.dest_pitch = dump_pitch;

  // Constant across the resolve, unlike the pitches and offsets.
  uint32_t push_constants[kEdramDumpShaderPushConstantCount] = {};
  push_constants[kEdramDumpShaderPushConstantResolveEdramInfo] =
      copy_shader_constants.dest_relative.edram_info.packed;
  push_constants[kEdramDumpShaderPushConstantResolveCoordinateInfo] =
      copy_shader_constants.dest_relative.coordinate_info.packed;
  push_constants[kEdramDumpShaderPushConstantResolveDestInfo] =
      copy_shader_constants.dest_relative.dest_info.value;
  push_constants[kEdramDumpShaderPushConstantResolveDestCoordinateInfo] =
      copy_shader_constants.dest_relative.dest_coordinate_info.packed;
  push_constants[kEdramDumpShaderPushConstantResolveDestBase] =
      copy_shader_constants.dest_base;
  push_constants[kEdramDumpShaderPushConstantResolveHeightDiv8] =
      resolve_info.height_div_8;

  bool encode_failed = false;
  for (size_t rectangle_index = 0;
       rectangle_index < rectangles.size() && !encode_failed;
       ++rectangle_index) {
    const ResolveCopyDumpRectangle& rect = rectangles[rectangle_index];
    RenderTargetKey rt_key =
        static_cast<MetalRenderTarget*>(rect.render_target)->key();
    MTL::Texture* source_texture = sources[rectangle_index].texture;
    MTL::Texture* stencil_texture = sources[rectangle_index].stencil_texture;

    MTL::Buffer* heap_buffer = nullptr;
    NS::UInteger heap_offset = 0;
    if (!command_processor_.AcquireSpirvArgumentBufferSlice(
            sizeof(IRDescriptorTableEntry) * 2, kInternalComputeSliceAlignment,
            &heap_buffer, &heap_offset)) {
      encode_failed = true;
      break;
    }
    auto* heap_entries = reinterpret_cast<IRDescriptorTableEntry*>(
        static_cast<uint8_t*>(heap_buffer->contents()) + heap_offset);
    std::memset(heap_entries, 0, sizeof(IRDescriptorTableEntry) * 2);
    IRDescriptorTableSetTexture(&heap_entries[0], source_texture, 0.0f, 0);
    if (stencil_texture) {
      IRDescriptorTableSetTexture(&heap_entries[1], stencil_texture, 0.0f, 0);
    }

    encoder->setComputePipelineState(sources[rectangle_index].pipeline);
    encoder->setBuffer(heap_buffer, heap_offset,
                       NS::UInteger(kIRDescriptorHeapBindPoint));
    // Nothing reached by GPU address is resident just from being written into
    // the argument buffer.
    encoder->useResource(dest_buffer, MTL::ResourceUsageWrite);
    encoder->useResource(source_texture, MTL::ResourceUsageRead);
    if (stencil_texture) {
      encoder->useResource(stencil_texture, MTL::ResourceUsageRead);
    }

    pitches.source_pitch = rt_key.GetPitchTiles();
    push_constants[kEdramDumpShaderPushConstantPitches] = pitches.pitches;

    // Tiles cover this many destination pixels, which is what the dispatch is
    // sized in.
    uint32_t tile_pixels_x =
        (xenos::kEdramTileWidthSamples >> uint32_t(rt_key.Is64bpp())) >>
        uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k4X);
    uint32_t tile_pixels_y =
        xenos::kEdramTileHeightSamples >>
        uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k2X);
    uint32_t pixels_per_thread =
        GetEdramDumpShaderResolvePixelsPerThread(rt_key.Is64bpp());

    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      uint32_t dispatch_first_tile = dump_base + dispatch.offset;
      EdramDumpShaderOffsets offsets;
      offsets.dispatch_first_tile =
          rt_key.GetNonWrappedTileIndex(dispatch_first_tile);
      offsets.source_base_tiles = rt_key.base_tiles;
      push_constants[kEdramDumpShaderPushConstantOffsets] = offsets.offsets;

      // Where the dispatch starts in the resolve's tile grid, which the
      // threads place themselves against.
      uint32_t dispatch_tile_relative =
          dispatch_first_tile -
          copy_shader_constants.dest_relative.edram_info.base_tiles;
      EdramDumpShaderResolveDispatchTile dispatch_tile;
      dispatch_tile.tile_x = dispatch_tile_relative % dump_pitch;
      dispatch_tile.tile_y = dispatch_tile_relative / dump_pitch;
      push_constants[kEdramDumpShaderPushConstantResolveDispatchTile] =
          dispatch_tile.packed;

      // A dispatch gets its own push constants and argument buffer: the GPU
      // reads them when it runs, so rewriting either between dispatches
      // already encoded into this pass would corrupt them.
      MTL::Buffer* push_constant_buffer = nullptr;
      NS::UInteger push_constant_offset = 0;
      MTL::Buffer* argument_buffer = nullptr;
      NS::UInteger argument_buffer_offset = 0;
      if (!command_processor_.AcquireSpirvArgumentBufferSlice(
              sizeof(push_constants), kInternalComputeSliceAlignment,
              &push_constant_buffer, &push_constant_offset) ||
          !command_processor_.AcquireSpirvArgumentBufferSlice(
              converter.internal_compute_argument_buffer_size(),
              kInternalComputeSliceAlignment, &argument_buffer,
              &argument_buffer_offset)) {
        encode_failed = true;
        break;
      }
      std::memcpy(static_cast<uint8_t*>(push_constant_buffer->contents()) +
                      push_constant_offset,
                  push_constants, sizeof(push_constants));

      auto* argument_buffer_data =
          static_cast<uint8_t*>(argument_buffer->contents()) +
          argument_buffer_offset;
      std::memset(argument_buffer_data, 0,
                  converter.internal_compute_argument_buffer_size());
      auto write_root_parameter =
          [&](MetalInternalComputeRootParameter parameter, uint64_t address) {
            std::memcpy(
                argument_buffer_data +
                    converter.internal_compute_root_parameter_offset(parameter),
                &address, sizeof(address));
          };
      write_root_parameter(MetalInternalComputeRootParameter::kDestUav,
                           uint64_t(dest_buffer->gpuAddress()));
      write_root_parameter(MetalInternalComputeRootParameter::kSourceTable,
                           uint64_t(heap_buffer->gpuAddress()) + heap_offset);
      write_root_parameter(
          MetalInternalComputeRootParameter::kPushConstants,
          uint64_t(push_constant_buffer->gpuAddress()) + push_constant_offset);
      encoder->setBuffer(argument_buffer, argument_buffer_offset,
                         NS::UInteger(kIRArgumentBufferBindPoint));

      uint32_t threads_x =
          (dispatch.width_tiles * tile_pixels_x + (pixels_per_thread - 1)) /
          pixels_per_thread;
      encoder->dispatchThreadgroups(
          MTL::Size::Make(
              (threads_x + (kEdramDumpShaderResolveThreadsPerGroupX - 1)) /
                  kEdramDumpShaderResolveThreadsPerGroupX,
              (dispatch.height_tiles * tile_pixels_y +
               (kEdramDumpShaderResolveThreadsPerGroupY - 1)) /
                  kEdramDumpShaderResolveThreadsPerGroupY,
              1),
          MTL::Size::Make(kEdramDumpShaderResolveThreadsPerGroupX,
                          kEdramDumpShaderResolveThreadsPerGroupY, 1));
    }
  }

  encoder->endEncoding();
  if (owns_command_buffer) {
    cmd->commit();
    cmd->waitUntilCompleted();
  }
  // cmd is autoreleased from commandBuffer() - do not release
  if (encode_failed) {
    // Whatever was encoded wrote part of the destination, but the round trip
    // the caller falls back to rewrites all of it, and later encoders in this
    // command buffer run after these.
    XELOGE(
        "MetalRenderTargetCache::DirectResolveRenderTargets: out of shader "
        "argument space, falling back to the EDRAM round trip");
    return false;
  }
  return true;
}

void MetalRenderTargetCache::DumpAllRenderTargetsToEdram() {
  DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount);
}

bool MetalRenderTargetCache::BeginEdramSnapshotReadback() {
  if (!edram_buffer_ || !device_) {
    return false;
  }
  if (!edram_snapshot_download_buffer_) {
    edram_snapshot_download_buffer_ = device_->newBuffer(
        xenos::kEdramSizeBytes, MTL::ResourceStorageModeShared);
    if (!edram_snapshot_download_buffer_) {
      XELOGE(
          "MetalRenderTargetCache: Failed to create the EDRAM snapshot "
          "download buffer");
      return false;
    }
    edram_snapshot_download_buffer_->setLabel(
        NS::String::string("EDRAM Snapshot Download", NS::UTF8StringEncoding));
  }

  // Nothing brackets this submission, so the copy is awaited here.
  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandBuffer* cmd = command_processor_.CreateAccountedCommandBuffer(
      MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
  MTL::BlitCommandEncoder* blit = cmd ? cmd->blitCommandEncoder() : nullptr;
  if (!blit) {
    command_processor_.DiscardAccountedCommandBuffer(
        cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetOther);
    return false;
  }
  blit->copyFromBuffer(edram_buffer_, 0, edram_snapshot_download_buffer_, 0,
                       xenos::kEdramSizeBytes);
  blit->endEncoding();
  cmd->commit();
  cmd->waitUntilCompleted();
  return true;
}

const void* MetalRenderTargetCache::MapEdramSnapshotReadback() {
  return edram_snapshot_download_buffer_
             ? edram_snapshot_download_buffer_->contents()
             : nullptr;
}

void MetalRenderTargetCache::EndEdramSnapshotReadback() {
  if (edram_snapshot_download_buffer_) {
    edram_snapshot_download_buffer_->release();
    edram_snapshot_download_buffer_ = nullptr;
  }
}

void MetalRenderTargetCache::DumpRenderTargets(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, MTL::CommandBuffer* command_buffer) {
  SCOPE_profile_cpu_f("gpu");
  std::vector<ResolveCopyDumpRectangle> rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, rectangles);
  if (rectangles.empty()) {
    XELOGW(
        "MetalRenderTargetCache::DumpRenderTargets: no rectangles for base={} "
        "row_length_used={} rows={} pitch={}",
        dump_base, dump_row_length_used, dump_rows, dump_pitch);
    return;
  }

  if (!edram_buffer_) {
    XELOGW(
        "MetalRenderTargetCache::DumpRenderTargets: EDRAM buffer not "
        "initialized, skipping GPU dump");
    return;
  }

  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    XELOGE("MetalRenderTargetCache::DumpRenderTargets: no command queue");
    return;
  }

  ScopedAutoreleasePool autorelease_pool;
  bool owns_command_buffer = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetDump);
    if (!cmd) {
      XELOGE("MetalRenderTargetCache::DumpRenderTargets: no command buffer");
      return;
    }
    owns_command_buffer = true;
  }

  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    XELOGE("MetalRenderTargetCache::DumpRenderTargets: no compute encoder");
    if (owns_command_buffer) {
      command_processor_.DiscardAccountedCommandBuffer(
          cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetDump);
    }
    return;
  }

  const MetalShaderConverter& converter =
      command_processor_.metal_shader_converter();
  const uint32_t scale_x = draw_resolution_scale_x();
  const uint32_t scale_y = draw_resolution_scale_y();

  EdramDumpShaderPitches pitches;
  pitches.dest_pitch = dump_pitch;

  for (const ResolveCopyDumpRectangle& rect : rectangles) {
    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt) {
      continue;
    }
    RenderTargetKey rt_key = rt->key();

    EdramDumpShaderKey shader_key;
    shader_key.is_depth = rt_key.is_depth;
    shader_key.resource_format = rt_key.resource_format;
    shader_key.msaa_samples = rt_key.msaa_samples;
    MTL::ComputePipelineState* dump_pipeline =
        GetOrCreateDumpPipeline(shader_key);
    if (!dump_pipeline) {
      continue;
    }

    // The source is the ownership transfer view, whose format the shader was
    // emitted against - an integer one for the formats where exact bits matter.
    MTL::Texture* source_texture = nullptr;
    MTL::Texture* stencil_texture = nullptr;
    if (rt_key.is_depth) {
      source_texture = rt->texture();
      stencil_texture = GetStencilTextureView(rt);
      if (!stencil_texture) {
        XELOGE(
            "MetalRenderTargetCache::DumpRenderTargets: no stencil view for a "
            "depth render target");
        continue;
      }
    } else {
      bool source_is_uint = false;
      GetColorOwnershipTransferPixelFormat(rt_key.GetColorFormat(),
                                           &source_is_uint);
      source_texture = source_is_uint ? rt->transfer_texture() : rt->texture();
    }
    if (!source_texture) {
      continue;
    }

    // The source textures reach the shader through a descriptor heap, and the
    // EDRAM buffer and push constants through the top-level argument buffer.
    MTL::Buffer* heap_buffer = nullptr;
    NS::UInteger heap_offset = 0;
    if (!command_processor_.AcquireSpirvArgumentBufferSlice(
            sizeof(IRDescriptorTableEntry) * 2, kInternalComputeSliceAlignment,
            &heap_buffer, &heap_offset)) {
      XELOGE(
          "MetalRenderTargetCache::DumpRenderTargets: failed to allocate the "
          "descriptor heap");
      break;
    }
    auto* heap_entries = reinterpret_cast<IRDescriptorTableEntry*>(
        static_cast<uint8_t*>(heap_buffer->contents()) + heap_offset);
    std::memset(heap_entries, 0, sizeof(IRDescriptorTableEntry) * 2);
    IRDescriptorTableSetTexture(&heap_entries[0], source_texture, 0.0f, 0);
    if (stencil_texture) {
      IRDescriptorTableSetTexture(&heap_entries[1], stencil_texture, 0.0f, 0);
    }

    encoder->setComputePipelineState(dump_pipeline);
    encoder->setBuffer(heap_buffer, heap_offset,
                       NS::UInteger(kIRDescriptorHeapBindPoint));
    // Nothing reached by GPU address is resident just from being written into
    // the argument buffer.
    encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);
    encoder->useResource(source_texture, MTL::ResourceUsageRead);
    if (stencil_texture) {
      encoder->useResource(stencil_texture, MTL::ResourceUsageRead);
    }

    pitches.source_pitch = rt_key.GetPitchTiles();

    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      EdramDumpShaderOffsets offsets;
      offsets.dispatch_first_tile =
          rt_key.GetNonWrappedTileIndex(dump_base + dispatch.offset);
      offsets.source_base_tiles = rt_key.base_tiles;

      // A dispatch gets its own push constants and argument buffer: the GPU
      // reads them when it runs, so rewriting either between dispatches
      // already encoded into this pass would corrupt them.
      MTL::Buffer* push_constant_buffer = nullptr;
      NS::UInteger push_constant_offset = 0;
      MTL::Buffer* argument_buffer = nullptr;
      NS::UInteger argument_buffer_offset = 0;
      if (!command_processor_.AcquireSpirvArgumentBufferSlice(
              sizeof(uint32_t) * kEdramDumpShaderPushConstantCount,
              kInternalComputeSliceAlignment, &push_constant_buffer,
              &push_constant_offset) ||
          !command_processor_.AcquireSpirvArgumentBufferSlice(
              converter.internal_compute_argument_buffer_size(),
              kInternalComputeSliceAlignment, &argument_buffer,
              &argument_buffer_offset)) {
        XELOGE(
            "MetalRenderTargetCache::DumpRenderTargets: failed to allocate the "
            "shader arguments");
        break;
      }

      auto* push_constant_data = reinterpret_cast<uint32_t*>(
          static_cast<uint8_t*>(push_constant_buffer->contents()) +
          push_constant_offset);
      push_constant_data[kEdramDumpShaderPushConstantPitches] = pitches.pitches;
      push_constant_data[kEdramDumpShaderPushConstantOffsets] = offsets.offsets;

      auto* argument_buffer_data =
          static_cast<uint8_t*>(argument_buffer->contents()) +
          argument_buffer_offset;
      std::memset(argument_buffer_data, 0,
                  converter.internal_compute_argument_buffer_size());
      auto write_root_parameter =
          [&](MetalInternalComputeRootParameter parameter, uint64_t address) {
            std::memcpy(
                argument_buffer_data +
                    converter.internal_compute_root_parameter_offset(parameter),
                &address, sizeof(address));
          };
      write_root_parameter(MetalInternalComputeRootParameter::kDestUav,
                           uint64_t(edram_buffer_->gpuAddress()));
      write_root_parameter(MetalInternalComputeRootParameter::kSourceTable,
                           uint64_t(heap_buffer->gpuAddress()) + heap_offset);
      write_root_parameter(
          MetalInternalComputeRootParameter::kPushConstants,
          uint64_t(push_constant_buffer->gpuAddress()) + push_constant_offset);
      encoder->setBuffer(argument_buffer, argument_buffer_offset,
                         NS::UInteger(kIRArgumentBufferBindPoint));

      encoder->dispatchThreadgroups(
          MTL::Size::Make((scale_x *
                               (xenos::kEdramTileWidthSamples >>
                                uint32_t(rt_key.Is64bpp())) *
                               dispatch.width_tiles +
                           (kEdramDumpShaderSamplesPerGroupX - 1)) /
                              kEdramDumpShaderSamplesPerGroupX,
                          (scale_y * xenos::kEdramTileHeightSamples *
                               dispatch.height_tiles +
                           (kEdramDumpShaderSamplesPerGroupY - 1)) /
                              kEdramDumpShaderSamplesPerGroupY,
                          1),
          MTL::Size::Make(kEdramDumpShaderSamplesPerGroupX,
                          kEdramDumpShaderSamplesPerGroupY, 1));
    }
  }

  encoder->endEncoding();
  if (owns_command_buffer) {
    cmd->commit();
    cmd->waitUntilCompleted();
  }
  // cmd is autoreleased from commandBuffer() - do not release
}

MTL::Library* MetalRenderTargetCache::GetOrCreateEdramLoadLibrary(bool msaa) {
  MTL::Library*& library =
      msaa ? edram_load_library_msaa_ : edram_load_library_;
  if (library) {
    return library;
  }

  static const char kEdramLoadShaderSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct EdramLoadConstants {
  uint base_tiles;
  uint pitch_tiles;
  uint format;
  uint format_is_64bpp;
  uint msaa_samples;
  uint sample_id;
  uint resolution_scale_x;
  uint resolution_scale_y;
};

struct VSOut {
  float4 position [[position]];
};

vertex VSOut edram_load_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

constant uint kXenosMsaaSamples1X = 0u;
constant uint kXenosMsaaSamples2X = 1u;
constant uint kXenosMsaaSamples4X = 2u;
constant uint kEdramTileCount = 2048u;

uint XeEdramOffsetInts(uint2 pixel_index, uint base_tiles, bool wrap,
                       uint pitch_tiles, uint msaa_samples, bool is_depth,
                       uint format_ints_log2, uint pixel_sample_index,
                       uint2 resolution_scale) {
  uint msaa_samples_x_log2 = (msaa_samples >= kXenosMsaaSamples4X) ? 1u : 0u;
  uint msaa_samples_y_log2 = (msaa_samples >= kXenosMsaaSamples2X) ? 1u : 0u;
  uint2 rt_sample_index =
      pixel_index << uint2(msaa_samples_x_log2, msaa_samples_y_log2);
  rt_sample_index +=
      (uint2(pixel_sample_index) >> uint2(1u, 0u)) & 1u;
  uint2 tile_size_at_32bpp = uint2(80u, 16u) * resolution_scale;
  uint2 tile_size_samples =
      tile_size_at_32bpp >> uint2(format_ints_log2, 0u);
  uint2 tile_offset_xy = rt_sample_index / tile_size_samples;
  base_tiles += tile_offset_xy.y * pitch_tiles + tile_offset_xy.x;
  rt_sample_index -= tile_offset_xy * tile_size_samples;
  if (is_depth) {
    uint tile_width_half = tile_size_samples.x >> 1u;
    rt_sample_index.x =
        uint(int(rt_sample_index.x) +
             ((rt_sample_index.x >= tile_width_half)
                  ? -int(tile_width_half)
                  : int(tile_width_half)));
  }
  uint address =
      base_tiles * (tile_size_at_32bpp.x * tile_size_at_32bpp.y) +
      ((rt_sample_index.y * tile_size_samples.x + rt_sample_index.x) <<
       format_ints_log2);
  if (wrap) {
    address %= tile_size_at_32bpp.x * tile_size_at_32bpp.y * kEdramTileCount;
  }
  return address;
}

float XeFloat7e3To32(uint f10) {
  f10 &= 0x3FFu;
  if (f10 == 0u) {
    return 0.0f;
  }
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = clz(mantissa) - 24u;
    exponent = uint(int(1) - int(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7Fu;
  }
  uint f32 = ((exponent + 124u) << 23u) | (mantissa << 16u);
  return as_type<float>(f32);
}

float4 XeUnpackR8G8B8A8UNorm(uint packed) {
  float4 value = float4(packed & 0xFFu, (packed >> 8u) & 0xFFu,
                        (packed >> 16u) & 0xFFu, packed >> 24u);
  return value * (1.0f / 255.0f);
}

float4 XeUnpackR10G10B10A2UNorm(uint packed) {
  float4 value = float4(packed & 0x3FFu, (packed >> 10u) & 0x3FFu,
                        (packed >> 20u) & 0x3FFu, (packed >> 30u) & 0x3u);
  return value * float4(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f,
                        1.0f / 3.0f);
}

float4 XeUnpackR10G10B10A2Float(uint packed) {
  float r = XeFloat7e3To32(packed & 0x3FFu);
  float g = XeFloat7e3To32((packed >> 10u) & 0x3FFu);
  float b = XeFloat7e3To32((packed >> 20u) & 0x3FFu);
  float a = float((packed >> 30u) & 0x3u) * (1.0f / 3.0f);
  return float4(r, g, b, a);
}

float2 XeUnpackR16G16Edram(uint packed) {
  int r = int(packed << 16u) >> 16u;
  int g = int(packed) >> 16u;
  float2 value = float2(float(r), float(g)) * (32.0f / 32767.0f);
  return max(value, float2(-1.0f));
}

float4 XeUnpackR16G16B16A16Edram(uint2 packed) {
  int r = int(packed.x << 16u) >> 16u;
  int g = int(packed.x) >> 16u;
  int b = int(packed.y << 16u) >> 16u;
  int a = int(packed.y) >> 16u;
  float4 value = float4(float(r), float(g), float(b), float(a)) *
                 (32.0f / 32767.0f);
  return max(value, float4(-1.0f));
}

float2 XeUnpackHalf2(uint packed) {
  return float2(as_type<half2>(packed));
}

float4 XeUnpackColor32bpp(uint format, uint packed) {
  switch (format) {
    case 0u:  // kXenosColorRenderTargetFormat_8_8_8_8
    case 1u:  // kXenosColorRenderTargetFormat_8_8_8_8_GAMMA
      return XeUnpackR8G8B8A8UNorm(packed);
    case 2u:  // kXenosColorRenderTargetFormat_2_10_10_10
    case 10u: // kXenosColorRenderTargetFormat_2_10_10_10_AS_10_10_10_10
      return XeUnpackR10G10B10A2UNorm(packed);
    case 3u:  // kXenosColorRenderTargetFormat_2_10_10_10_FLOAT
    case 12u: // kXenosColorRenderTargetFormat_2_10_10_10_FLOAT_AS_16_16_16_16
      return XeUnpackR10G10B10A2Float(packed);
    case 4u: {  // kXenosColorRenderTargetFormat_16_16
      float2 rg = XeUnpackR16G16Edram(packed);
      return float4(rg, 0.0f, 1.0f);
    }
    case 6u: {  // kXenosColorRenderTargetFormat_16_16_FLOAT
      float2 rg = XeUnpackHalf2(packed);
      return float4(rg, 0.0f, 1.0f);
    }
    case 14u:  // kXenosColorRenderTargetFormat_32_FLOAT
      return float4(as_type<float>(packed), 0.0f, 0.0f, 1.0f);
    default:
      return float4(0.0f);
  }
}

float4 XeUnpackColor64bpp(uint format, uint2 packed) {
  switch (format) {
    case 5u:  // kXenosColorRenderTargetFormat_16_16_16_16
      return XeUnpackR16G16B16A16Edram(packed);
    case 7u: {  // kXenosColorRenderTargetFormat_16_16_16_16_FLOAT
      float2 rg = XeUnpackHalf2(packed.x);
      float2 ba = XeUnpackHalf2(packed.y);
      return float4(rg, ba);
    }
    case 15u:  // kXenosColorRenderTargetFormat_32_32_FLOAT
      return float4(as_type<float>(packed.x), as_type<float>(packed.y),
                    0.0f, 0.0f);
    default:
      return float4(0.0f);
  }
}

struct EdramLoadOut {
  float4 color [[color(0)]];
#if XE_EDRAM_LOAD_MSAA
  uint sample_mask [[sample_mask]];
#endif
};

fragment EdramLoadOut edram_load_ps(
    VSOut in [[stage_in]],
    constant EdramLoadConstants& constants [[buffer(0)]],
    device const uint* edram [[buffer(1)]]) {
  uint2 pixel = uint2(in.position.xy);
  uint format_ints_log2 = constants.format_is_64bpp;
  uint address = XeEdramOffsetInts(
      pixel, constants.base_tiles, true, constants.pitch_tiles,
      constants.msaa_samples, false, format_ints_log2, constants.sample_id,
      uint2(constants.resolution_scale_x, constants.resolution_scale_y));
  float4 color;
  if (constants.format_is_64bpp != 0u) {
    uint2 packed = uint2(edram[address], edram[address + 1u]);
    color = XeUnpackColor64bpp(constants.format, packed);
  } else {
    color = XeUnpackColor32bpp(constants.format, edram[address]);
  }
  EdramLoadOut out;
  out.color = color;
#if XE_EDRAM_LOAD_MSAA
  out.sample_mask = 1u << (constants.sample_id & 0x1Fu);
#endif
  return out;
}
)METAL";

  std::string source;
  source.reserve(sizeof(kEdramLoadShaderSource) + 32);
  source.append(msaa ? "#define XE_EDRAM_LOAD_MSAA 1\n"
                     : "#define XE_EDRAM_LOAD_MSAA 0\n");
  source.append(kEdramLoadShaderSource);

  NS::Error* error = nullptr;
  auto source_str = NS::String::string(source.c_str(), NS::UTF8StringEncoding);
  library = device_->newLibrary(source_str, nullptr, &error);
  if (!library) {
    XELOGE("Metal: failed to compile edram load shader: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return library;
}

MTL::RenderPipelineState* MetalRenderTargetCache::GetOrCreateEdramLoadPipeline(
    MTL::PixelFormat dest_format, uint32_t sample_count) {
  uint64_t key = uint64_t(dest_format) | (uint64_t(sample_count) << 32);
  auto it = edram_load_pipelines_.find(key);
  if (it != edram_load_pipelines_.end()) {
    return it->second;
  }

  bool msaa = sample_count > 1;
  MTL::Library* lib = GetOrCreateEdramLoadLibrary(msaa);
  if (!lib) {
    return nullptr;
  }

  NS::String* vs_name =
      NS::String::string("edram_load_vs", NS::UTF8StringEncoding);
  NS::String* ps_name =
      NS::String::string("edram_load_ps", NS::UTF8StringEncoding);
  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    XELOGE("Metal: edram load missing shader entrypoints");
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->colorAttachments()->object(0)->setPixelFormat(dest_format);
  desc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
  desc->setSampleCount(sample_count);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);
  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    XELOGE("Metal: failed to create edram load pipeline: {}",
           error ? error->localizedDescription()->utf8String() : "unknown");
    return nullptr;
  }

  edram_load_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool MetalRenderTargetCache::Resolve(Memory& memory, uint32_t& written_address,
                                     uint32_t& written_length,
                                     MTL::CommandBuffer* command_buffer) {
  SCOPE_profile_cpu_f("gpu");
  // Resolving reads the render targets, so anything still queued for a draw
  // pass has to have been applied to them first.
  if (!FlushPendingDrawPassTransfers()) {
    return false;
  }

  written_address = 0;
  written_length = 0;
  const RegisterFile& regs = register_file();
  draw_util::ResolveInfo resolve_info;

  // Fixed16 formats may be truncated to -1..1 when backed by SNORM.
  bool fixed_rg16_trunc = IsFixedRG16TruncatedToMinus1To1();
  bool fixed_rgba16_trunc = IsFixedRGBA16TruncatedToMinus1To1();

  if (!trace_writer_) {
    XELOGE("MetalRenderTargetCache::Resolve: trace_writer_ is null");
    return false;
  }

  if (!draw_util::GetResolveInfo(regs, memory, *trace_writer_,
                                 draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), fixed_rg16_trunc,
                                 fixed_rgba16_trunc, resolve_info)) {
    XELOGE("MetalRenderTargetCache::Resolve: GetResolveInfo failed");
    return false;
  }

  // Nothing to do.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  bool is_depth = resolve_info.IsCopyingDepth();

  if (resolve_info.copy_dest_extent_length &&
      resolve_info.rb_copy_control.copy_command ==
          xenos::CopyCommand::kConvertTo1111) {
    XELOGW("MetalResolve: resolve copy command 2 is not supported");
    resolve_info.copy_dest_extent_length = 0;
  }

  if (!resolve_info.copy_dest_extent_length) {
    return true;
  }

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  const auto& coord = resolve_info.coordinate_info;
  uint32_t resolve_width = coord.width_div_8 * 8;
  uint32_t resolve_height = resolve_info.height_div_8 * 8;

  // Compute the EDRAM tile span for this resolve.
  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch);

  draw_util::ResolveCopyShaderConstants copy_constants;
  uint32_t group_count_x = 0, group_count_y = 0;
  draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
      draw_resolution_scale_x(), draw_resolution_scale_y(), copy_constants,
      group_count_x, group_count_y);

  // Read the render targets straight into shared memory where the copy
  // wouldn't have converted anything. Otherwise match D3D12/Vulkan: dump host
  // RT ownership into EDRAM, then resolve from EDRAM to shared memory.
  // Resolve-time blend fallback is not correct because blending state is
  // per-draw, not per-resolve.
  // Metal's direct resolve only ever writes shared memory - it has neither the
  // native 1x1 copy nor a binding of the resolution-scaled resolve buffer, so
  // anything under scaling takes the EDRAM round trip.
  bool resolved_directly = false;
  if (::cvars::direct_host_resolve && !draw_resolution_scaled &&
      GetDirectResolveEligibility(resolve_info, copy_shader) ==
          DirectResolveEligibility::kEligible) {
    resolved_directly = DirectResolveRenderTargets(
        resolve_info, copy_constants, dump_base, dump_row_length_used,
        dump_rows, dump_pitch, command_buffer);
  }

  if (!resolved_directly) {
    DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                      command_buffer);
  }

  uint32_t dest_base = resolve_info.copy_dest_base;
  uint32_t dest_local_start = resolve_info.copy_dest_extent_start - dest_base;
  uint32_t dest_local_end =
      dest_local_start + resolve_info.copy_dest_extent_length;

  command_processor_.SetSwapDestSwap(
      dest_base, resolve_info.copy_dest_info.copy_dest_swap);

  // Color resolves are 8888; depth resolves may use different destination
  // formats, so only apply the 4-byte-per-pixel assumption to color.
  uint32_t bytes_per_pixel = 4;

  // Bookkeeping both paths need once the destination has been written.
  auto finish_resolve = [&]() {
    written_address = resolve_info.copy_dest_extent_start;
    written_length = resolve_info.copy_dest_extent_length;

    // Marks the range GPU-written in shared memory too, which invalidates the
    // textures overlapping it so they reload the resolved data.
    if (auto* tex_cache = command_processor_.texture_cache()) {
      tex_cache->MarkRangeAsResolved(written_address, written_length,
                                     draw_resolution_scaled);
    }

    bool clear_depth = resolve_info.IsClearingDepth();
    bool clear_color = resolve_info.IsClearingColor();
    if (clear_depth || clear_color) {
      Transfer::Rectangle clear_rectangle;
      RenderTarget* clear_targets[2] = {};
      std::vector<Transfer> clear_transfers[2];
      if (PrepareHostRenderTargetsResolveClear(
              resolve_info, clear_rectangle, clear_targets[0],
              clear_transfers[0], clear_targets[1], clear_transfers[1])) {
        uint64_t clear_values[2];
        clear_values[0] = resolve_info.rb_depth_clear;
        clear_values[1] = resolve_info.rb_color_clear |
                          (uint64_t(resolve_info.rb_color_clear_lo) << 32);
        PerformTransfersAndResolveClears(2, clear_targets, clear_transfers,
                                         clear_values, &clear_rectangle,
                                         command_buffer);
      }
    }
    return true;
  };

  if (resolved_directly) {
    return finish_resolve();
  }

  // Resolve out of EDRAM, matching D3D12/Vulkan behavior for the supported
  // cases.
  if (edram_buffer_) {
    // Select the appropriate Metal pipeline for this shader.
    MTL::ComputePipelineState* pipeline = nullptr;
    if (draw_resolution_scaled) {
      switch (copy_shader) {
        case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
          pipeline = resolve_fast_32bpp_1x2xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
          pipeline = resolve_fast_32bpp_4xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
          pipeline = resolve_fast_64bpp_1x2xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
          pipeline = resolve_fast_64bpp_4xmsaa_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull8bpp:
          pipeline = resolve_full_8bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull16bpp:
          pipeline = resolve_full_16bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull32bpp:
          pipeline = resolve_full_32bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull64bpp:
          pipeline = resolve_full_64bpp_scaled_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull128bpp:
          pipeline = resolve_full_128bpp_scaled_pipeline_;
          break;
        default:
          pipeline = nullptr;
          break;
      }
    } else {
      switch (copy_shader) {
        case draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA:
          pipeline = resolve_fast_32bpp_1x2xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA:
          pipeline = resolve_fast_32bpp_4xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA:
          pipeline = resolve_fast_64bpp_1x2xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA:
          pipeline = resolve_fast_64bpp_4xmsaa_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull8bpp:
          pipeline = resolve_full_8bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull16bpp:
          pipeline = resolve_full_16bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull32bpp:
          pipeline = resolve_full_32bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull64bpp:
          pipeline = resolve_full_64bpp_pipeline_;
          break;
        case draw_util::ResolveCopyShaderIndex::kFull128bpp:
          pipeline = resolve_full_128bpp_pipeline_;
          break;
        default:
          pipeline = nullptr;
          break;
      }
    }
    if (draw_resolution_scaled && !pipeline) {
      static uint32_t missing_scaled_pipeline_log_count = 0;
      if (missing_scaled_pipeline_log_count < 8) {
        ++missing_scaled_pipeline_log_count;
        XELOGW("MetalResolve: scaled resolve pipeline missing for shader {}",
               int(copy_shader));
      }
    }

    if (pipeline && group_count_x && group_count_y) {
      // Preserve the guest pitch from GetCopyShader: destination extents and
      // access tracking use the same layout computed by GetResolveInfo.
      auto* shared = command_processor_.shared_memory();
      auto* texture_cache = command_processor_.texture_cache();
      MTL::Buffer* dest_buffer = nullptr;
      size_t dest_buffer_offset = 0;
      size_t dest_buffer_length = 0;
      const uint8_t* shared_bytes = nullptr;
      uint32_t scaled_range_length = 0;
      if (draw_resolution_scaled) {
        auto* metal_texture_cache =
            texture_cache ? static_cast<MetalTextureCache*>(texture_cache)
                          : nullptr;
        if (!metal_texture_cache) {
          XELOGE("MetalResolve: missing MetalTextureCache for scaled resolve");
          return false;
        }
        uint32_t range_length = resolve_info.copy_dest_extent_start -
                                resolve_info.copy_dest_base +
                                resolve_info.copy_dest_extent_length;
        scaled_range_length = range_length;
        if (!metal_texture_cache->EnsureScaledResolveMemoryCommitted(
                resolve_info.copy_dest_extent_start,
                resolve_info.copy_dest_extent_length) ||
            !metal_texture_cache->MakeScaledResolveRangeCurrent(
                resolve_info.copy_dest_base, range_length) ||
            !metal_texture_cache->GetCurrentScaledResolveBuffer(
                dest_buffer, dest_buffer_offset, dest_buffer_length)) {
          XELOGE("MetalResolve: failed to select scaled resolve buffer");
          return false;
        }
        (void)dest_buffer_length;
      } else {
        dest_buffer = shared ? shared->GetBuffer() : nullptr;
        if (!dest_buffer) {
          XELOGE("MetalResolve: missing shared memory buffer");
          return false;
        }
        // Request the destination shared memory range before the GPU write,
        // mirroring D3D12/Vulkan behavior. This ensures pages are committed and
        // any CPU data is uploaded before the GPU overwrites it.
        if (!shared->RequestRange(resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length)) {
          XELOGE(
              "MetalRenderTargetCache::Resolve: RequestRange failed for "
              "0x{:08X} len {}",
              resolve_info.copy_dest_extent_start,
              resolve_info.copy_dest_extent_length);
          return false;
        }

        shared_bytes = static_cast<const uint8_t*>(dest_buffer->contents());
      }
      if (draw_resolution_scaled) {
      }

      MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();

      if (!queue) {
        XELOGE(
            "MetalRenderTargetCache::Resolve: no command queue for GPU path");
      } else {
        ScopedAutoreleasePool autorelease_pool;
        bool owns_command_buffer = false;
        MTL::CommandBuffer* cmd = command_buffer;
        if (!cmd) {
          cmd = command_processor_.CreateAccountedCommandBuffer(
              MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
          if (!cmd) {
            XELOGE(
                "MetalRenderTargetCache::Resolve: failed to get command "
                "buffer for GPU path");
            cmd = nullptr;
          }
          owns_command_buffer = true;
        }
        if (cmd) {
          MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
          if (!encoder) {
            XELOGE(
                "MetalRenderTargetCache::Resolve: failed to get compute "
                "encoder for GPU path");
            if (owns_command_buffer) {
              command_processor_.DiscardAccountedCommandBuffer(
                  cmd, MetalCommandProcessor::CommandBufferKind::
                           kRenderTargetResolve);
            }
          } else {
            encoder->setComputePipelineState(pipeline);

            // Buffer 0: push constants
            if (draw_resolution_scaled) {
              encoder->setBytes(&copy_constants.dest_relative,
                                sizeof(copy_constants.dest_relative), 0);
            } else {
              encoder->setBytes(&copy_constants, sizeof(copy_constants), 0);
            }

            // Buffer 1: destination memory (shared or scaled resolve).
            encoder->setBuffer(dest_buffer, dest_buffer_offset, 1);

            // Buffer 2: EDRAM source buffer.
            encoder->setBuffer(edram_buffer_, 0, 2);
            encoder->useResource(dest_buffer, MTL::ResourceUsageWrite);
            encoder->useResource(edram_buffer_, MTL::ResourceUsageRead);

            encoder->dispatchThreadgroups(
                MTL::Size::Make(group_count_x, group_count_y, 1),
                MTL::Size::Make(8, 8, 1));

            encoder->endEncoding();
            if (owns_command_buffer) {
              cmd->commit();
              cmd->waitUntilCompleted();
            }
            // cmd is autoreleased from commandBuffer() - do not release

            return finish_resolve();
          }
        }
      }
    }
  }

  XELOGE(
      "MetalRenderTargetCache::Resolve: no valid GPU resolve shader / pipeline "
      "for this configuration");
  return false;
}

EdramTransferShaderKey MetalRenderTargetCache::GetTransferShaderKey(
    RenderTargetKey source_key, RenderTargetKey dest_key,
    const RenderTargetKey* host_depth_source_key,
    bool host_depth_source_is_copy, bool stencil_bit,
    uint32_t dest_color_rt_index) const {
  EdramTransferShaderKey shader_key;
  shader_key.source_msaa_samples = source_key.msaa_samples;
  shader_key.dest_msaa_samples = dest_key.msaa_samples;
  shader_key.source_resource_format = source_key.resource_format;
  shader_key.dest_resource_format = dest_key.resource_format;
  shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
  shader_key.dest_color_rt_index = dest_color_rt_index;

  if (stencil_bit) {
    shader_key.mode = source_key.is_depth
                          ? EdramTransferMode::kDepthToStencilBit
                          : EdramTransferMode::kColorToStencilBit;
  } else if (dest_key.is_depth) {
    if (host_depth_source_key) {
      // Reading the host depth back out of the EDRAM buffer is a mode of its
      // own rather than a flag, so the shader declares a buffer instead of a
      // texture for it.
      if (host_depth_source_is_copy) {
        shader_key.mode =
            source_key.is_depth
                ? EdramTransferMode::kDepthAndHostDepthCopyToDepth
                : EdramTransferMode::kColorAndHostDepthCopyToDepth;
      } else {
        shader_key.mode = source_key.is_depth
                              ? EdramTransferMode::kDepthAndHostDepthToDepth
                              : EdramTransferMode::kColorAndHostDepthToDepth;
        shader_key.host_depth_source_msaa_samples =
            host_depth_source_key->msaa_samples;
      }
    } else {
      shader_key.mode = source_key.is_depth ? EdramTransferMode::kDepthToDepth
                                            : EdramTransferMode::kColorToDepth;
    }
  } else {
    shader_key.mode = source_key.is_depth ? EdramTransferMode::kDepthToColor
                                          : EdramTransferMode::kColorToColor;
  }

  shader_key.value_convert =
      IsTransferValueConverted7e3And8888(source_key, dest_key) ? 1 : 0;
  return shader_key;
}

bool MetalRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    MTL::CommandBuffer* command_buffer,
    MTL::RenderCommandEncoder* active_render_encoder,
    MTL::RenderPassDescriptor* active_render_pass_descriptor,
    DrawPassTransferEncoderMutationMask* mutations_out) {
  SCOPE_profile_cpu_f("gpu");
  if (mutations_out) {
    *mutations_out = kDrawPassTransferEncoderMutationNone;
  }
  if (!render_targets || !render_target_transfers) {
    return false;
  }

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;
  // Resolve clears build a pass of their own around a clear load action, which
  // the guest's already-started pass cannot provide.
  bool use_active_render_encoder = active_render_encoder != nullptr;
  if (use_active_render_encoder &&
      (resolve_clear_needed || !active_render_pass_descriptor)) {
    return false;
  }
  auto mark_encoder_mutation =
      [&](DrawPassTransferEncoderMutationMask mutations) {
        if (use_active_render_encoder && mutations_out) {
          *mutations_out |= mutations;
        }
      };
  TransferAttachmentFormats active_attachment_formats;
  if (use_active_render_encoder &&
      !GetActiveTransferAttachmentFormats(active_render_pass_descriptor,
                                          active_attachment_formats)) {
    return false;
  }
  bool any_work = false;
  bool host_depth_store_needed = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    if (resolve_clear_needed) {
      any_work = true;
    }
    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty()) {
      continue;
    }
    any_work = true;
    if (!dest_rt->key().is_depth) {
      continue;
    }
    for (const Transfer& transfer : transfers) {
      if (transfer.host_depth_source == dest_rt) {
        host_depth_store_needed = true;
        break;
      }
    }
  }
  if (!any_work) {
    return true;
  }
  // The host depth store is a compute dispatch, which cannot be encoded into a
  // render pass.
  if (use_active_render_encoder && host_depth_store_needed) {
    return false;
  }

  // Encoding into the guest's pass touches no encoder of its own, so it needs
  // no command buffer either.
  MTL::CommandBuffer* cmd = nullptr;
  if (!use_active_render_encoder) {
    cmd = command_buffer ? command_buffer
                         : command_processor_.EnsureCommandBuffer();
    if (!cmd) {
      XELOGE(
          "MetalRenderTargetCache::PerformTransfersAndResolveClears: no "
          "command buffer");
      return false;
    }
    command_processor_.EndRenderEncoder();
  }

  uint32_t scale_x = draw_resolution_scale_x();
  uint32_t scale_y = draw_resolution_scale_y();
  uint32_t tile_width_samples =
      xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples =
      xenos::kEdramTileHeightSamples * draw_resolution_scale_y();
  uint32_t depth_round = (!::cvars::depth_float24_convert_in_pixel_shader &&
                          ::cvars::depth_float24_round)
                             ? 1u
                             : 0u;

  // Host depth store pass (dest depth where host depth source == dest).
  bool host_depth_store_dispatched = false;
  if (host_depth_store_needed) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      RenderTarget* dest_rt = render_targets[i];
      if (!dest_rt) {
        continue;
      }
      RenderTargetKey dest_key = dest_rt->key();
      if (!dest_key.is_depth) {
        continue;
      }
      const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
      for (const Transfer& transfer : depth_transfers) {
        if (transfer.host_depth_source != dest_rt) {
          continue;
        }
        auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
        MTL::Texture* depth_texture = dest_metal_rt->texture();
        if (!depth_texture || !edram_buffer_) {
          continue;
        }
        size_t pipeline_index = size_t(dest_key.msaa_samples);
        if (pipeline_index >= xe::countof(host_depth_store_pipelines_) ||
            !host_depth_store_pipelines_[pipeline_index]) {
          XELOGE(
              "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
              "missing host depth store pipeline for msaa={}",
              uint32_t(dest_key.msaa_samples));
          continue;
        }
        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        uint32_t rectangle_count = transfer.GetRectangles(
            dest_key, rectangles, resolve_clear_rectangle);
        if (!rectangle_count) {
          continue;
        }
        HostDepthStoreRenderTargetConstant render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_supported_);
        MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
        if (!encoder) {
          XELOGE(
              "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
              "failed to create host depth store encoder");
          continue;
        }
        encoder->setComputePipelineState(
            host_depth_store_pipelines_[pipeline_index]);
        encoder->setBuffer(edram_buffer_, 0, 1);
        encoder->setTexture(depth_texture, 0);
        encoder->useResource(edram_buffer_, MTL::ResourceUsageWrite);
        encoder->useResource(depth_texture, MTL::ResourceUsageRead);
        for (uint32_t rect_index = 0; rect_index < rectangle_count;
             ++rect_index) {
          uint32_t group_count_x = 0;
          uint32_t group_count_y = 0;
          HostDepthStoreRectangleConstant rectangle_constant;
          GetHostDepthStoreRectangleInfo(
              rectangles[rect_index], dest_key.msaa_samples, rectangle_constant,
              group_count_x, group_count_y);
          if (!group_count_x || !group_count_y) {
            continue;
          }
          HostDepthStoreConstants constants = {};
          constants.rectangle = rectangle_constant;
          constants.render_target = render_target_constant;
          encoder->setBytes(&constants, sizeof(constants), 0);
          encoder->dispatchThreadgroups(
              MTL::Size::Make(group_count_x, group_count_y, 1),
              MTL::Size::Make(8, 8, 1));
          host_depth_store_dispatched = true;
        }
        encoder->endEncoding();
      }
      break;
    }
  }

  bool any_transfers_done = false;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
    if (dest_metal_rt->needs_initial_clear()) {
      dest_metal_rt->SetNeedsInitialClear(false);
      render_pass_descriptor_dirty_ = true;
    }
    RenderTargetKey dest_key = dest_metal_rt->key();
    bool dest_is_depth = dest_key.is_depth;

    bool dest_is_uint = false;
    MTL::PixelFormat dest_pixel_format =
        dest_is_depth ? GetDepthPixelFormat(dest_key.GetDepthFormat())
                      : GetColorOwnershipTransferPixelFormat(
                            dest_key.GetColorFormat(), &dest_is_uint);

    MTL::Texture* dest_texture = dest_is_depth
                                     ? dest_metal_rt->texture()
                                     : dest_metal_rt->transfer_texture();
    if (!dest_texture) {
      XELOGW(
          "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
          "Destination RT {} has no texture",
          i);
      continue;
    }
    if (dest_is_depth) {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer depth must use resource pixel format");
    } else {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer color must use ownership pixel format");
    }

    // The destination has to be an attachment of the pass already under way,
    // bound through the same texture the transfer draws write.
    uint32_t active_color_attachment_index = 0;
    if (use_active_render_encoder) {
      if (dest_is_depth) {
        auto* depth_attachment =
            active_render_pass_descriptor->depthAttachment();
        MTL::Texture* depth_texture =
            depth_attachment ? depth_attachment->texture() : nullptr;
        if (i != 0 || depth_texture != dest_texture ||
            active_attachment_formats.depth_attachment_format !=
                dest_pixel_format) {
          return false;
        }
        if (dest_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
            dest_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
          auto* stencil_attachment =
              active_render_pass_descriptor->stencilAttachment();
          MTL::Texture* stencil_texture =
              stencil_attachment ? stencil_attachment->texture() : nullptr;
          if (stencil_texture != depth_texture ||
              active_attachment_formats.stencil_attachment_format !=
                  dest_pixel_format) {
            return false;
          }
        }
      } else {
        if (!i || i > xenos::kMaxColorRenderTargets) {
          return false;
        }
        active_color_attachment_index = i - 1;
        auto* color_attachments =
            active_render_pass_descriptor->colorAttachments();
        auto* color_attachment =
            color_attachments
                ? color_attachments->object(active_color_attachment_index)
                : nullptr;
        if (!color_attachment || color_attachment->texture() != dest_texture ||
            active_attachment_formats
                    .color_attachment_formats[active_color_attachment_index] !=
                dest_pixel_format) {
          return false;
        }
      }
    }
    const TransferColorAttachmentFormats* active_color_formats =
        use_active_render_encoder
            ? &active_attachment_formats.color_attachment_formats
            : nullptr;
    MTL::PixelFormat active_depth_format =
        use_active_render_encoder
            ? active_attachment_formats.depth_attachment_format
            : MTL::PixelFormatInvalid;
    MTL::PixelFormat active_stencil_format =
        use_active_render_encoder
            ? active_attachment_formats.stencil_attachment_format
            : MTL::PixelFormatInvalid;

    uint32_t dest_sample_count = MsaaSamplesToCount(dest_key.msaa_samples);
    uint32_t dest_width = uint32_t(dest_texture->width());
    uint32_t dest_height = uint32_t(dest_texture->height());

    auto get_scaled_rect = [&](const Transfer::Rectangle& rect,
                               uint32_t& scaled_x, uint32_t& scaled_y,
                               uint32_t& scaled_width,
                               uint32_t& scaled_height) -> bool {
      uint32_t rect_x = rect.x_pixels * scale_x;
      uint32_t rect_y = rect.y_pixels * scale_y;
      uint32_t rect_width = rect.width_pixels * scale_x;
      uint32_t rect_height = rect.height_pixels * scale_y;
      if (rect_x >= dest_width || rect_y >= dest_height) {
        return false;
      }
      rect_width = std::min(rect_width, dest_width - rect_x);
      rect_height = std::min(rect_height, dest_height - rect_y);
      if (!rect_width || !rect_height) {
        return false;
      }
      scaled_x = rect_x;
      scaled_y = rect_y;
      scaled_width = rect_width;
      scaled_height = rect_height;
      return true;
    };

    auto set_rect_viewport = [&](MTL::RenderCommandEncoder* encoder,
                                 const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      MTL::Viewport vp;
      vp.originX = double(scaled_x);
      vp.originY = double(scaled_y);
      vp.width = double(scaled_width);
      vp.height = double(scaled_height);
      vp.znear = 0.0;
      vp.zfar = 1.0;
      encoder->setViewport(vp);
      mark_encoder_mutation(kDrawPassTransferEncoderMutationViewport);
      MTL::ScissorRect scissor;
      scissor.x = scaled_x;
      scissor.y = scaled_y;
      scissor.width = scaled_width;
      scissor.height = scaled_height;
      encoder->setScissorRect(scissor);
      mark_encoder_mutation(kDrawPassTransferEncoderMutationScissor);
      return true;
    };

    std::vector<Transfer> filtered_transfers;
    bool used_blit = false;
    MTL::BlitCommandEncoder* blit_encoder = nullptr;
    auto ensure_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
      if (!blit_encoder) {
        blit_encoder = cmd->blitCommandEncoder();
      }
      return blit_encoder;
    };

    // Fast path: when source/dest share compatible EDRAM layout and format,
    // use a blit instead of shader-based transfers. A blit needs an encoder of
    // its own, which is exactly the split the draw-pass path exists to avoid.
    if (!use_active_render_encoder && !transfers.empty()) {
      auto try_blit_transfer = [&](const Transfer& transfer) -> bool {
        auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
        if (!source_rt || transfer.host_depth_source) {
          return false;
        }

        RenderTargetKey source_key = source_rt->key();
        if (dest_is_depth != source_key.is_depth) {
          return false;
        }
        if (source_key.resource_format != dest_key.resource_format ||
            source_key.msaa_samples != dest_key.msaa_samples ||
            source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp) {
          return false;
        }

        if (source_key.base_tiles != dest_key.base_tiles ||
            source_key.next_period != dest_key.next_period) {
          return false;
        }

        MTL::Texture* source_texture = dest_is_depth
                                           ? source_rt->texture()
                                           : source_rt->transfer_texture();
        if (!source_texture) {
          return false;
        }
        if (!dest_is_depth) {
          MTL::PixelFormat expected_format =
              GetColorOwnershipTransferPixelFormat(source_key.GetColorFormat(),
                                                   nullptr);
          assert_true(source_texture->pixelFormat() == expected_format,
                      "Transfer source must use ownership pixel format");
        }
        if (source_texture->pixelFormat() != dest_texture->pixelFormat() ||
            source_texture->sampleCount() != dest_texture->sampleCount() ||
            source_texture->sampleCount() != 1 ||
            source_texture->width() != dest_width ||
            source_texture->height() != dest_height) {
          return false;
        }

        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        uint32_t rectangle_count = transfer.GetRectangles(
            dest_key, dest_key.Is64bpp(), rectangles, resolve_clear_rectangle);
        if (!rectangle_count) {
          return false;
        }

        MTL::BlitCommandEncoder* blit = ensure_blit_encoder();
        if (!blit) {
          return false;
        }

        for (uint32_t rect_index = 0; rect_index < rectangle_count;
             ++rect_index) {
          uint32_t scaled_x = 0;
          uint32_t scaled_y = 0;
          uint32_t scaled_width = 0;
          uint32_t scaled_height = 0;
          if (!get_scaled_rect(rectangles[rect_index], scaled_x, scaled_y,
                               scaled_width, scaled_height)) {
            continue;
          }
          MTL::Origin origin = MTL::Origin::Make(scaled_x, scaled_y, 0);
          MTL::Size size = MTL::Size::Make(scaled_width, scaled_height, 1);
          blit->copyFromTexture(source_texture, 0, 0, origin, size,
                                dest_texture, 0, 0, origin);
        }

        used_blit = true;
        any_transfers_done = true;
        return true;
      };

      for (const Transfer& transfer : transfers) {
        if (!try_blit_transfer(transfer)) {
          filtered_transfers.push_back(transfer);
        }
      }
    }

    if (blit_encoder) {
      blit_encoder->endEncoding();
    }

    const bool disable_transfer_shaders = false;
    const std::vector<Transfer>& transfers_for_shaders =
        used_blit ? filtered_transfers : transfers;

    auto is_full_target_rectangle =
        [&](const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      return !scaled_x && !scaled_y && scaled_width == dest_width &&
             scaled_height == dest_height;
    };

    auto transfers_fully_overwrite_target = [&]() -> bool {
      if (transfers_for_shaders.empty()) {
        return false;
      }
      for (const Transfer& transfer : transfers_for_shaders) {
        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        uint32_t rectangle_count = transfer.GetRectangles(
            dest_key, dest_key.Is64bpp(), rectangles, resolve_clear_rectangle);
        if (rectangle_count != 1 || !is_full_target_rectangle(rectangles[0])) {
          return false;
        }
      }
      return true;
    };

    bool resolve_clear_fully_overwrites_target = false;
    if (resolve_clear_needed && resolve_clear_rectangle) {
      resolve_clear_fully_overwrites_target =
          is_full_target_rectangle(*resolve_clear_rectangle);
    }

    bool resolve_clear_via_load_action = false;
    MTL::ClearColor resolve_clear_color = MTL::ClearColor(0.0, 0.0, 0.0, 0.0);
    double resolve_clear_depth = 1.0;
    uint32_t resolve_clear_stencil = 0;
    if (resolve_clear_needed && resolve_clear_fully_overwrites_target) {
      const uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_depth = xenos::UNorm24To32(depth_guest_clear_value);
            resolve_clear_via_load_action = true;
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            resolve_clear_depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            resolve_clear_via_load_action = true;
            break;
        }
        resolve_clear_stencil = uint32_t(clear_value) & 0xFF;
      } else {
        TransferClearColorFloatConstants float_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        GetColorOwnershipTransferPixelFormat(dest_key.GetColorFormat(),
                                             &clear_is_uint);
        if (!clear_is_uint && !clear_via_drawing) {
          resolve_clear_color = MTL::ClearColor(
              float_constants.color[0], float_constants.color[1],
              float_constants.color[2], float_constants.color[3]);
          resolve_clear_via_load_action = true;
        }
      }
    }

    // Prefer DontCare on transfer-pass loads only when destination contents are
    // provably fully overwritten by this pass.
    bool transfer_pass_load_dontcare = false;
    if (resolve_clear_fully_overwrites_target &&
        !resolve_clear_via_load_action) {
      transfer_pass_load_dontcare = true;
    }
    if (!transfer_pass_load_dontcare && !resolve_clear_needed) {
      transfer_pass_load_dontcare = transfers_fully_overwrite_target();
    }
    MTL::LoadAction transfer_load_action = MTL::LoadActionLoad;
    if (resolve_clear_via_load_action) {
      transfer_load_action = MTL::LoadActionClear;
    } else if (transfer_pass_load_dontcare) {
      transfer_load_action = MTL::LoadActionDontCare;
    }

    MTL::RenderCommandEncoder* transfer_encoder = nullptr;
    auto ensure_transfer_encoder = [&]() -> MTL::RenderCommandEncoder* {
      if (transfer_encoder) {
        return transfer_encoder;
      }
      if (use_active_render_encoder) {
        transfer_encoder = active_render_encoder;
        // Whatever the guest left on the encoder must not cull, wireframe or
        // depth-bias the full-viewport transfer draws.
        transfer_encoder->setCullMode(MTL::CullModeNone);
        transfer_encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
        transfer_encoder->setDepthBias(0.0f, 0.0f, 0.0f);
        transfer_encoder->setDepthClipMode(MTL::DepthClipModeClip);
        mark_encoder_mutation(kDrawPassTransferEncoderMutationRasterizer);
        return transfer_encoder;
      }
      MTL::RenderPassDescriptor* rp =
          MTL::RenderPassDescriptor::renderPassDescriptor();
      if (dest_is_depth) {
        auto* da = rp->depthAttachment();
        da->setTexture(dest_texture);
        da->setLoadAction(transfer_load_action);
        da->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          da->setClearDepth(resolve_clear_depth);
        }
        if (dest_pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
            dest_pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
          auto* sa = rp->stencilAttachment();
          sa->setTexture(dest_texture);
          sa->setLoadAction(transfer_load_action);
          sa->setStoreAction(MTL::StoreActionStore);
          if (transfer_load_action == MTL::LoadActionClear) {
            sa->setClearStencil(resolve_clear_stencil);
          }
        }
      } else {
        auto* ca = rp->colorAttachments()->object(0);
        ca->setTexture(dest_texture);
        ca->setLoadAction(transfer_load_action);
        ca->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          ca->setClearColor(resolve_clear_color);
        }
      }
      transfer_encoder = cmd->renderCommandEncoder(rp);
      return transfer_encoder;
    };

    if (!transfers_for_shaders.empty() && disable_transfer_shaders) {
      static uint32_t transfer_shader_skip_log_count = 0;
      if (transfer_shader_skip_log_count < 8) {
        ++transfer_shader_skip_log_count;
        XELOGW(
            "MetalRenderTargetCache::PerformTransfersAndResolveClears: "
            "transfer shaders disabled; skipping {} transfers for RT {}",
            transfers_for_shaders.size(), i);
      }
    } else if (!transfers_for_shaders.empty()) {
      // The depth draw carries the guest stencil where the device allows it,
      // which leaves nothing for the clear and the per-bit draws to do.
      bool use_native_stencil_output =
          dest_is_depth && UseNativeStencilOutputInTransfers();
      bool need_stencil_bit_draws = dest_is_depth && !use_native_stencil_output;
      bool stencil_clear_needed = need_stencil_bit_draws;

      transfer_invocations_.clear();
      transfer_invocations_.reserve(transfers_for_shaders.size() *
                                    (need_stencil_bit_draws ? 2 : 1));

      for (const Transfer& transfer : transfers_for_shaders) {
        if (transfer.source) {
          auto* source = static_cast<MetalRenderTarget*>(transfer.source);
          source->SetTemporarySortIndex(UINT32_MAX);
        }
        if (transfer.host_depth_source) {
          auto* host_depth =
              static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          host_depth->SetTemporarySortIndex(UINT32_MAX);
        }
      }

      uint32_t rt_sort_index = 0;
      auto ensure_sort_index = [&](MetalRenderTarget* rt) {
        if (rt && rt->temporary_sort_index() == UINT32_MAX) {
          rt->SetTemporarySortIndex(rt_sort_index++);
        }
      };

      for (uint32_t pass = 0; pass <= uint32_t(need_stencil_bit_draws);
           ++pass) {
        for (const Transfer& transfer : transfers_for_shaders) {
          if (!transfer.source) {
            continue;
          }
          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          auto* host_depth_rt =
              pass
                  ? nullptr
                  : static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          ensure_sort_index(source_rt);
          ensure_sort_index(host_depth_rt);

          RenderTargetKey source_key = source_rt->key();
          RenderTargetKey host_depth_key;
          if (host_depth_rt) {
            host_depth_key = host_depth_rt->key();
          }
          EdramTransferShaderKey shader_key = GetTransferShaderKey(
              source_key, dest_key, host_depth_rt ? &host_depth_key : nullptr,
              host_depth_rt == dest_metal_rt, pass != 0,
              active_color_attachment_index);

          transfer_invocations_.emplace_back(transfer, shader_key);
          if (pass) {
            transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }

      std::sort(transfer_invocations_.begin(), transfer_invocations_.end());

      if (stencil_clear_needed) {
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(
                dest_pixel_format, false, true, dest_sample_count, 0,
                active_color_formats, active_depth_format,
                active_stencil_format);
        MTL::DepthStencilState* stencil_clear_state =
            GetTransferStencilClearState();
        if (clear_pipeline && stencil_clear_state) {
          MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
          if (encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = 0.0f;
            encoder->setRenderPipelineState(clear_pipeline);
            encoder->setDepthStencilState(stencil_clear_state);
            encoder->setStencilReferenceValue(0);
            encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            mark_encoder_mutation(
                kDrawPassTransferEncoderMutationPipeline |
                kDrawPassTransferEncoderMutationDepthStencil |
                kDrawPassTransferEncoderMutationStencilReference |
                kDrawPassTransferEncoderMutationFragmentSlot0);
            for (const Transfer& transfer : transfers_for_shaders) {
              Transfer::Rectangle
                  rectangles[Transfer::kMaxRectanglesWithCutout];
              uint32_t rectangle_count =
                  transfer.GetRectangles(dest_key, dest_key.Is64bpp(),
                                         rectangles, resolve_clear_rectangle);
              for (uint32_t rect_index = 0; rect_index < rectangle_count;
                   ++rect_index) {
                if (!set_rect_viewport(encoder, rectangles[rect_index])) {
                  continue;
                }
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                        NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
      MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
      if (encoder) {
        bool transfer_viewport_full_set = false;
        MTL::ScissorRect last_transfer_scissor = {};
        bool last_transfer_scissor_valid = false;
        MTL::RenderPipelineState* last_transfer_pipeline = nullptr;
        MTL::DepthStencilState* last_transfer_depth_state = nullptr;
        bool last_transfer_stencil_reference_valid = false;
        uint32_t last_transfer_stencil_reference = 0;
        bool transfer_vertex_constants_valid = false;
        TransferVertexConstants last_transfer_vertex_constants = {};
        bool last_transfer_vertex_bytes_1_valid = false;
        TransferRectInstance last_transfer_vertex_bytes_1 = {};
        auto bind_transfer_pipeline = [&](MTL::RenderPipelineState* pipeline) {
          if (last_transfer_pipeline != pipeline) {
            encoder->setRenderPipelineState(pipeline);
            mark_encoder_mutation(kDrawPassTransferEncoderMutationPipeline);
            last_transfer_pipeline = pipeline;
          }
        };
        auto bind_transfer_depth_state = [&](MTL::DepthStencilState* state) {
          if (last_transfer_depth_state != state) {
            encoder->setDepthStencilState(state);
            mark_encoder_mutation(kDrawPassTransferEncoderMutationDepthStencil);
            last_transfer_depth_state = state;
          }
        };
        const bool transfer_uses_spirv_cross =
            cvars::metal_transfer_spirv_cross;
        MTL::Texture* set_textures[2][2] = {};
        MTL::Buffer* host_depth_buffer = nullptr;
        auto bind_transfer_source_texture = [&](uint32_t set, uint32_t binding,
                                                MTL::Texture* texture) {
          set_textures[set][binding] = texture;
        };
        auto bind_transfer_host_depth_buffer = [&](MTL::Buffer* buffer) {
          host_depth_buffer = buffer;
        };
        // A draw reads its heap, push constants and argument buffer when it
        // runs, so every sample's draw needs its own copy of whatever differs
        // between them rather than one rewritten between draws already encoded
        // into this pass. Only the push constants do differ, so they and the
        // argument buffers pointing at them are laid out for all the samples in
        // one allocation each, and the heap, the residency and the textures are
        // bound once for the whole group.
        uint32_t transfer_push_constants[kTransferSampleDrawMax]
                                        [kTransferPushConstantDwordMax];
        uint32_t transfer_push_constant_count = 0;
        MTL::Buffer* transfer_argument_buffer = nullptr;
        NS::UInteger transfer_argument_buffer_offset = 0;
        uint32_t transfer_argument_buffer_stride = 0;
        auto bind_transfer_fragment_resources =
            [&](const EdramTransferShaderKey& shader_key,
                const EdramTransferAddressConstant& address,
                const EdramTransferAddressConstant& host_depth_address,
                uint32_t stencil_mask, uint32_t sample_draw_count) -> bool {
          assert_true(sample_draw_count <= kTransferSampleDrawMax);
          const EdramTransferPipelineLayoutInfo& layout_info =
              kEdramTransferPipelineLayoutInfos[size_t(
                  kEdramTransferModes[size_t(shader_key.mode)]
                      .pipeline_layout)];

          // Only the dwords the layout declares are present, in enum order,
          // with the sample index after them when the shader takes it there.
          uint32_t
              shared_push_constants[kEdramTransferUsedPushConstantDwordCount];
          uint32_t shared_push_constant_count = 0;
          if (layout_info.used_push_constant_dwords &
              kEdramTransferUsedPushConstantDwordHostDepthAddressBit) {
            shared_push_constants[shared_push_constant_count++] =
                host_depth_address.constant;
          }
          if (layout_info.used_push_constant_dwords &
              kEdramTransferUsedPushConstantDwordAddressBit) {
            shared_push_constants[shared_push_constant_count++] =
                address.constant;
          }
          if (layout_info.used_push_constant_dwords &
              kEdramTransferUsedPushConstantDwordStencilMaskBit) {
            shared_push_constants[shared_push_constant_count++] = stencil_mask;
          }
          const bool draws_samples_separately =
              TransferDrawsSamplesSeparately(shader_key);
          transfer_push_constant_count =
              shared_push_constant_count + (draws_samples_separately ? 1 : 0);
          for (uint32_t sample_draw = 0; sample_draw < sample_draw_count;
               ++sample_draw) {
            std::memcpy(transfer_push_constants[sample_draw],
                        shared_push_constants,
                        sizeof(uint32_t) * shared_push_constant_count);
            if (draws_samples_separately) {
              // Emulating 2x MSAA as samples 0 and 3 of a 4-sample attachment
              // when there is no native 2x, the way the addressing expects.
              bool emulated_2x_upper_sample = sample_draw_count == 2 &&
                                              !msaa_2x_supported_ &&
                                              sample_draw == 1;
              transfer_push_constants[sample_draw][shared_push_constant_count] =
                  emulated_2x_upper_sample ? 3u : sample_draw;
            }
          }

          if (transfer_uses_spirv_cross) {
            MTL::Texture* textures[kTransferMslTextureCount] = {};
            for (uint32_t set = 0; set < kTransferDescriptorSetCount; ++set) {
              for (uint32_t binding = 0; binding < kTransferBindingsPerSet;
                   ++binding) {
                textures[TransferMslTextureIndex(set, binding)] =
                    set_textures[set][binding];
              }
            }
            encoder->setFragmentTextures(
                textures, NS::Range::Make(0, kTransferMslTextureCount));
            if (host_depth_buffer) {
              encoder->setFragmentBuffer(host_depth_buffer, 0,
                                         kTransferMslHostDepthBufferIndex);
            }
            mark_encoder_mutation(
                kDrawPassTransferEncoderMutationFragmentTextures |
                kDrawPassTransferEncoderMutationFragmentSlot0 |
                kDrawPassTransferEncoderMutationFragmentSlot1);
            return true;
          }

          const MetalShaderConverter& converter =
              command_processor_.metal_shader_converter();
          const uint32_t push_constant_stride =
              xe::align(std::max<uint32_t>(
                            sizeof(uint32_t) * transfer_push_constant_count,
                            sizeof(uint32_t)),
                        kInternalComputeSliceAlignment);
          transfer_argument_buffer_stride =
              xe::align(converter.internal_graphics_argument_buffer_size(),
                        kInternalComputeSliceAlignment);
          MTL::Buffer* push_constant_buffer = nullptr;
          NS::UInteger push_constant_offset = 0;
          if (!command_processor_.AcquireSpirvArgumentBufferSlice(
                  push_constant_stride * sample_draw_count,
                  kInternalComputeSliceAlignment, &push_constant_buffer,
                  &push_constant_offset) ||
              !command_processor_.AcquireSpirvArgumentBufferSlice(
                  transfer_argument_buffer_stride * sample_draw_count,
                  kInternalComputeSliceAlignment, &transfer_argument_buffer,
                  &transfer_argument_buffer_offset)) {
            return false;
          }

          // One heap slice per set the layout declares, in the dense order the
          // emitter numbers them.
          static const MetalInternalGraphicsRootParameter kTableParameters[2] =
              {
                  MetalInternalGraphicsRootParameter::kSourceTable0,
                  MetalInternalGraphicsRootParameter::kSourceTable1,
              };
          MTL::Buffer* heap_buffer = nullptr;
          NS::UInteger heap_offset = 0;
          auto set_used = [&](uint32_t set) {
            return set_textures[set][0] || set_textures[set][1];
          };
          uint32_t table_count = 0;
          for (uint32_t set = 0; set < 2; ++set) {
            if (set_used(set)) {
              ++table_count;
            }
          }
          uint64_t table_addresses[2] = {};
          if (table_count) {
            if (!command_processor_.AcquireSpirvArgumentBufferSlice(
                    sizeof(IRDescriptorTableEntry) * 2 * table_count,
                    kInternalComputeSliceAlignment, &heap_buffer,
                    &heap_offset)) {
              return false;
            }
            auto* heap_entries = reinterpret_cast<IRDescriptorTableEntry*>(
                static_cast<uint8_t*>(heap_buffer->contents()) + heap_offset);
            std::memset(heap_entries, 0,
                        sizeof(IRDescriptorTableEntry) * 2 * table_count);
            uint32_t table_index = 0;
            for (uint32_t set = 0; set < 2; ++set) {
              if (!set_used(set)) {
                continue;
              }
              IRDescriptorTableEntry* entries =
                  heap_entries + size_t(table_index) * 2;
              for (uint32_t binding = 0; binding < 2; ++binding) {
                MTL::Texture* texture = set_textures[set][binding];
                if (!texture) {
                  continue;
                }
                IRDescriptorTableSetTexture(&entries[binding], texture, 0.0f,
                                            0);
                encoder->useResource(texture, MTL::ResourceUsageRead,
                                     MTL::RenderStageFragment);
              }
              table_addresses[table_index] =
                  uint64_t(heap_buffer->gpuAddress()) + heap_offset +
                  sizeof(IRDescriptorTableEntry) * 2 * table_index;
              ++table_index;
            }
            encoder->setFragmentBuffer(
                heap_buffer, heap_offset,
                NS::UInteger(kIRDescriptorHeapBindPoint));
          }
          if (host_depth_buffer) {
            encoder->useResource(host_depth_buffer, MTL::ResourceUsageRead,
                                 MTL::RenderStageFragment);
          }

          for (uint32_t sample_draw = 0; sample_draw < sample_draw_count;
               ++sample_draw) {
            NS::UInteger sample_push_constant_offset =
                push_constant_offset + push_constant_stride * sample_draw;
            if (transfer_push_constant_count) {
              std::memcpy(
                  static_cast<uint8_t*>(push_constant_buffer->contents()) +
                      sample_push_constant_offset,
                  transfer_push_constants[sample_draw],
                  sizeof(uint32_t) * transfer_push_constant_count);
            }
            auto* argument_buffer_data =
                static_cast<uint8_t*>(transfer_argument_buffer->contents()) +
                transfer_argument_buffer_offset +
                transfer_argument_buffer_stride * sample_draw;
            std::memset(argument_buffer_data, 0,
                        converter.internal_graphics_argument_buffer_size());
            auto write_root_parameter =
                [&](MetalInternalGraphicsRootParameter parameter,
                    uint64_t address_value) {
                  std::memcpy(
                      argument_buffer_data +
                          converter.internal_graphics_root_parameter_offset(
                              parameter),
                      &address_value, sizeof(address_value));
                };
            for (uint32_t table_index = 0; table_index < table_count;
                 ++table_index) {
              write_root_parameter(kTableParameters[table_index],
                                   table_addresses[table_index]);
            }
            if (host_depth_buffer) {
              write_root_parameter(
                  MetalInternalGraphicsRootParameter::kHostDepthBufferUav,
                  uint64_t(host_depth_buffer->gpuAddress()));
            }
            write_root_parameter(
                MetalInternalGraphicsRootParameter::kPushConstants,
                uint64_t(push_constant_buffer->gpuAddress()) +
                    sample_push_constant_offset);
          }
          mark_encoder_mutation(
              kDrawPassTransferEncoderMutationFragmentTextures |
              kDrawPassTransferEncoderMutationFragmentSlot0 |
              kDrawPassTransferEncoderMutationFragmentSlot1);
          return true;
        };
        auto bind_transfer_fragment_sample = [&](uint32_t sample_draw) {
          if (transfer_uses_spirv_cross) {
            if (transfer_push_constant_count) {
              encoder->setFragmentBytes(
                  transfer_push_constants[sample_draw],
                  sizeof(uint32_t) * transfer_push_constant_count,
                  kTransferMslPushConstantBufferIndex);
            }
            return;
          }
          encoder->setFragmentBuffer(
              transfer_argument_buffer,
              transfer_argument_buffer_offset +
                  transfer_argument_buffer_stride * sample_draw,
              NS::UInteger(kIRArgumentBufferBindPoint));
        };
        auto bind_transfer_stencil_reference = [&](uint32_t reference) {
          if (!last_transfer_stencil_reference_valid ||
              last_transfer_stencil_reference != reference) {
            encoder->setStencilReferenceValue(reference);
            mark_encoder_mutation(
                kDrawPassTransferEncoderMutationStencilReference);
            last_transfer_stencil_reference = reference;
            last_transfer_stencil_reference_valid = true;
          }
        };
        auto bind_transfer_vertex_constants =
            [&](const TransferVertexConstants& constants) {
              if (!transfer_vertex_constants_valid ||
                  std::memcmp(&last_transfer_vertex_constants, &constants,
                              sizeof(constants)) != 0) {
                encoder->setVertexBytes(&constants, sizeof(constants), 0);
                mark_encoder_mutation(
                    kDrawPassTransferEncoderMutationVertexSlot0);
                last_transfer_vertex_constants = constants;
                transfer_vertex_constants_valid = true;
              }
            };
        auto bind_transfer_scissor = [&](const MTL::ScissorRect& scissor) {
          if (!last_transfer_scissor_valid ||
              last_transfer_scissor.x != scissor.x ||
              last_transfer_scissor.y != scissor.y ||
              last_transfer_scissor.width != scissor.width ||
              last_transfer_scissor.height != scissor.height) {
            encoder->setScissorRect(scissor);
            mark_encoder_mutation(kDrawPassTransferEncoderMutationScissor);
            last_transfer_scissor = scissor;
            last_transfer_scissor_valid = true;
          }
        };
        auto bind_transfer_vertex_bytes_1 =
            [&](const TransferRectInstance& rect_instance) {
              if (!last_transfer_vertex_bytes_1_valid ||
                  std::memcmp(&last_transfer_vertex_bytes_1, &rect_instance,
                              sizeof(rect_instance)) != 0) {
                encoder->setVertexBytes(&rect_instance, sizeof(rect_instance),
                                        1);
                mark_encoder_mutation(
                    kDrawPassTransferEncoderMutationVertexSlot1);
                last_transfer_vertex_bytes_1 = rect_instance;
                last_transfer_vertex_bytes_1_valid = true;
              }
            };
        auto bind_transfer_vertex_bytes_1_span =
            [&](const TransferRectInstance* rect_instances,
                uint32_t rect_instance_count) {
              if (!rect_instances || !rect_instance_count) {
                return;
              }
              encoder->setVertexBytes(
                  rect_instances,
                  size_t(rect_instance_count) * sizeof(TransferRectInstance),
                  1);
              mark_encoder_mutation(
                  kDrawPassTransferEncoderMutationVertexSlot1);
              last_transfer_vertex_bytes_1_valid = false;
            };
        auto set_full_transfer_viewport_scissor = [&]() {
          if (!transfer_viewport_full_set) {
            MTL::Viewport vp;
            vp.originX = 0.0;
            vp.originY = 0.0;
            vp.width = double(dest_width);
            vp.height = double(dest_height);
            vp.znear = 0.0;
            vp.zfar = 1.0;
            encoder->setViewport(vp);
            mark_encoder_mutation(kDrawPassTransferEncoderMutationViewport);
            transfer_viewport_full_set = true;
          }
          MTL::ScissorRect scissor;
          scissor.x = 0;
          scissor.y = 0;
          scissor.width = dest_width;
          scissor.height = dest_height;
          bind_transfer_scissor(scissor);
        };

        std::vector<Transfer::Rectangle> merged_transfer_rectangles;
        for (size_t invocation_index = 0;
             invocation_index < transfer_invocations_.size();) {
          const auto& invocation = transfer_invocations_[invocation_index];
          size_t merged_invocation_end = invocation_index + 1;
          while (merged_invocation_end < transfer_invocations_.size() &&
                 invocation.CanBeMergedIntoOneDraw(
                     transfer_invocations_[merged_invocation_end])) {
            ++merged_invocation_end;
          }

          merged_transfer_rectangles.clear();
          merged_transfer_rectangles.reserve(
              (merged_invocation_end - invocation_index) *
              Transfer::kMaxRectanglesWithCutout);
          for (size_t merged_index = invocation_index;
               merged_index < merged_invocation_end; ++merged_index) {
            Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
            uint32_t rectangle_count =
                transfer_invocations_[merged_index].transfer.GetRectangles(
                    dest_key, dest_key.Is64bpp(), rectangles,
                    resolve_clear_rectangle);
            for (uint32_t rect_index = 0; rect_index < rectangle_count;
                 ++rect_index) {
              merged_transfer_rectangles.push_back(rectangles[rect_index]);
            }
          }
          invocation_index = merged_invocation_end;
          if (merged_transfer_rectangles.empty()) {
            continue;
          }

          const Transfer& transfer = invocation.transfer;
          const EdramTransferShaderKey& shader_key = invocation.shader_key;
          const EdramTransferModeInfo& mode_info =
              kEdramTransferModes[size_t(shader_key.mode)];
          bool is_stencil_bit =
              mode_info.output == EdramTransferOutput::kStencilBit;
          bool writes_stencil_with_depth =
              use_native_stencil_output &&
              mode_info.output == EdramTransferOutput::kDepth;
          bool needs_source_stencil =
              !EdramTransferSourceIsColor(shader_key.mode) &&
              (mode_info.output == EdramTransferOutput::kColor ||
               is_stencil_bit || writes_stencil_with_depth);

          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          if (!source_rt) {
            continue;
          }

          RenderTargetKey source_key = source_rt->key();
          bool source_is_uint = false;
          MTL::PixelFormat source_transfer_format = MTL::PixelFormatInvalid;
          if (EdramTransferSourceIsColor(shader_key.mode)) {
            source_transfer_format = GetColorOwnershipTransferPixelFormat(
                source_key.GetColorFormat(), &source_is_uint);
          }

          if (is_stencil_bit) {
            // Depth/stencil state set per-bit below.
          } else if (dest_is_depth) {
            bind_transfer_depth_state(
                writes_stencil_with_depth
                    ? GetTransferDepthAndStencilOutputState()
                    : GetTransferDepthStencilState(true));
          } else {
            MTL::DepthStencilState* no_depth_state =
                GetTransferNoDepthStencilState();
            if (!no_depth_state) {
              continue;
            }
            bind_transfer_depth_state(no_depth_state);
          }

          // The host depth set is declared before the source one, so a mode
          // that reads both puts the previous owner in set 1.
          std::memset(set_textures, 0, sizeof(set_textures));
          host_depth_buffer = nullptr;
          uint32_t source_set =
              EdramTransferUsesHostDepth(shader_key.mode) ? 1u : 0u;
          if (EdramTransferSourceIsColor(shader_key.mode)) {
            MTL::Texture* source_texture = source_rt->transfer_texture();
            if (!source_texture) {
              continue;
            }
            assert_true(source_texture->pixelFormat() == source_transfer_format,
                        "Transfer source must use ownership pixel format");
            bind_transfer_source_texture(source_set, 0, source_texture);
          } else {
            MTL::Texture* depth_texture = source_rt->texture();
            if (!depth_texture) {
              continue;
            }
            bind_transfer_source_texture(source_set, 0, depth_texture);
            if (needs_source_stencil) {
              MTL::Texture* stencil_texture = GetStencilTextureView(source_rt);
              if (!stencil_texture) {
                continue;
              }
              bind_transfer_source_texture(source_set, 1, stencil_texture);
            }
          }

          if (EdramTransferUsesHostDepth(shader_key.mode)) {
            if (EdramTransferHostDepthIsCopy(shader_key.mode)) {
              // The copy modes read the previous owner back out of the EDRAM
              // buffer rather than binding it as a texture.
              MTL::Buffer* buffer =
                  edram_buffer_ ? edram_buffer_ : GetTransferDummyBuffer();
              if (!buffer) {
                continue;
              }
              bind_transfer_host_depth_buffer(buffer);
            } else {
              auto* host_depth_rt =
                  static_cast<MetalRenderTarget*>(transfer.host_depth_source);
              MTL::Texture* host_depth_texture =
                  host_depth_rt ? host_depth_rt->texture() : nullptr;
              if (!host_depth_texture) {
                continue;
              }
              bind_transfer_source_texture(0, 0, host_depth_texture);
            }
          }

          // Everything the old MSL took as a runtime value - formats, sample
          // counts, tile dimensions - the emitter bakes into the shader, so
          // only the addressing is left to pass.
          EdramTransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_key.GetPitchTiles();
          address_constant.source_pitch = source_key.GetPitchTiles();
          address_constant.source_to_dest = (uint32_t(dest_key.base_tiles) -
                                             uint32_t(source_key.base_tiles)) &
                                            (xenos::kEdramTileCount - 1);
          address_constant.source_next_period = source_key.next_period;
          EdramTransferAddressConstant host_depth_address_constant;
          if (EdramTransferUsesHostDepth(shader_key.mode) &&
              !EdramTransferHostDepthIsCopy(shader_key.mode)) {
            auto* host_depth_rt =
                static_cast<MetalRenderTarget*>(transfer.host_depth_source);
            if (host_depth_rt) {
              RenderTargetKey host_depth_key = host_depth_rt->key();
              host_depth_address_constant.dest_pitch = dest_key.GetPitchTiles();
              host_depth_address_constant.source_pitch =
                  host_depth_key.GetPitchTiles();
              host_depth_address_constant.source_to_dest =
                  (uint32_t(dest_key.base_tiles) -
                   uint32_t(host_depth_key.base_tiles)) &
                  (xenos::kEdramTileCount - 1);
            }
          }
          TransferVertexConstants vertex_constants;
          vertex_constants.pixel_to_ndc_x =
              dest_width ? (2.0f / float(dest_width)) : 0.0f;
          vertex_constants.pixel_to_ndc_y =
              dest_height ? (2.0f / float(dest_height)) : 0.0f;

          const uint32_t rectangle_count =
              uint32_t(merged_transfer_rectangles.size());
          std::vector<TransferRectInstance> rect_instance_list;
          rect_instance_list.reserve(rectangle_count);
          for (uint32_t rect_index = 0; rect_index < rectangle_count;
               ++rect_index) {
            uint32_t scaled_x = 0;
            uint32_t scaled_y = 0;
            uint32_t scaled_width = 0;
            uint32_t scaled_height = 0;
            if (!get_scaled_rect(merged_transfer_rectangles[rect_index],
                                 scaled_x, scaled_y, scaled_width,
                                 scaled_height) ||
                !scaled_width || !scaled_height) {
              continue;
            }
            TransferRectInstance rect_instance = {};
            rect_instance.origin_x = float(scaled_x);
            rect_instance.origin_y = float(scaled_y);
            rect_instance.size_x = float(scaled_width);
            rect_instance.size_y = float(scaled_height);
            rect_instance_list.push_back(rect_instance);
          }

          MTL::RenderPipelineState* pipeline = GetOrCreateTransferPipelines(
              shader_key, dest_pixel_format, dest_is_uint,
              writes_stencil_with_depth, active_color_attachment_index,
              active_color_formats, active_depth_format, active_stencil_format);
          if (!pipeline) {
            continue;
          }
          bind_transfer_pipeline(pipeline);
          bind_transfer_vertex_constants(vertex_constants);

          // Either the shader runs per sample and one draw covers all of them,
          // or it takes the sample index from the push constants and each
          // sample gets its own draw through the same pipeline.
          const uint32_t sample_draw_count =
              TransferDrawsSamplesSeparately(shader_key)
                  ? MsaaSamplesToCount(dest_key.msaa_samples)
                  : 1;

          auto draw_transfer_rects = [&]() {
            if (!rect_instance_list.empty()) {
              set_full_transfer_viewport_scissor();
              constexpr uint32_t kTransferRectInlineBatchMax = 240;
              const TransferRectInstance* rect_instances =
                  rect_instance_list.data();
              uint32_t rect_instances_remaining =
                  uint32_t(rect_instance_list.size());
              while (rect_instances_remaining) {
                uint32_t batch_count = std::min(rect_instances_remaining,
                                                kTransferRectInlineBatchMax);
                if (batch_count == 1) {
                  bind_transfer_vertex_bytes_1(*rect_instances);
                } else {
                  bind_transfer_vertex_bytes_1_span(rect_instances,
                                                    batch_count);
                }
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                                        NS::UInteger(0), NS::UInteger(4),
                                        NS::UInteger(batch_count));
                rect_instances += batch_count;
                rect_instances_remaining -= batch_count;
              }
            }
          };

          auto draw_transfer = [&](uint32_t stencil_mask) -> bool {
            if (!bind_transfer_fragment_resources(
                    shader_key, address_constant, host_depth_address_constant,
                    stencil_mask, sample_draw_count)) {
              return false;
            }
            for (uint32_t sample_draw = 0; sample_draw < sample_draw_count;
                 ++sample_draw) {
              bind_transfer_fragment_sample(sample_draw);
              draw_transfer_rects();
            }
            return true;
          };

          if (is_stencil_bit) {
            for (uint32_t bit = 0; bit < 8; ++bit) {
              MTL::DepthStencilState* stencil_state =
                  GetTransferStencilBitState(bit);
              if (!stencil_state) {
                continue;
              }
              bind_transfer_depth_state(stencil_state);
              bind_transfer_stencil_reference(uint32_t(1) << bit);
              if (!draw_transfer(uint32_t(1) << bit)) {
                break;
              }
            }
          } else {
            draw_transfer(0);
          }
          any_transfers_done = true;
        }
      }
    }

    if (resolve_clear_needed && !resolve_clear_via_load_action) {
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        float depth_host_clear_value = 0.0f;
        switch (dest_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            depth_host_clear_value =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(
                dest_pixel_format, false, true, dest_sample_count, 0,
                active_color_formats, active_depth_format,
                active_stencil_format);
        MTL::DepthStencilState* clear_state = GetTransferDepthClearState();
        if (clear_pipeline && clear_state) {
          MTL::RenderCommandEncoder* clear_encoder = ensure_transfer_encoder();
          if (clear_encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = depth_host_clear_value;
            clear_encoder->setRenderPipelineState(clear_pipeline);
            clear_encoder->setDepthStencilState(clear_state);
            clear_encoder->setStencilReferenceValue(uint32_t(clear_value) &
                                                    0xFF);
            clear_encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            mark_encoder_mutation(
                kDrawPassTransferEncoderMutationPipeline |
                kDrawPassTransferEncoderMutationDepthStencil |
                kDrawPassTransferEncoderMutationStencilReference |
                kDrawPassTransferEncoderMutationFragmentSlot0);
            Transfer::Rectangle clear_rect = *resolve_clear_rectangle;
            if (set_rect_viewport(clear_encoder, clear_rect)) {
              clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                            NS::UInteger(0), NS::UInteger(3));
            }
          }
        }
      } else {
        TransferClearColorFloatConstants float_constants = {};
        TransferClearColorUintConstants uint_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            if (gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        MTL::PixelFormat clear_format = GetColorOwnershipTransferPixelFormat(
            dest_key.GetColorFormat(), &clear_is_uint);
        MTL::Texture* clear_texture = dest_metal_rt->transfer_texture();
        bool clear_use_uint = clear_is_uint;

        if (clear_use_uint) {
          switch (dest_key.GetColorFormat()) {
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = (uint32_t(clear_value >> 32)) & 0xFFFF;
              uint_constants.color[3] =
                  (uint32_t(clear_value >> 32) >> 16) & 0xFFFF;
              break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = 0;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = uint32_t(clear_value >> 32);
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            default:
              break;
          }
        }

        if (clear_via_drawing && clear_use_uint) {
          uint_constants.color[0] = uint32_t(clear_value);
          uint_constants.color[1] = uint32_t(clear_value >> 32);
          uint_constants.color[2] = 0;
          uint_constants.color[3] = 0;
        }

        if (clear_texture) {
          MTL::RenderPipelineState* clear_pipeline =
              GetOrCreateTransferClearPipeline(
                  clear_format, clear_use_uint, false, dest_sample_count,
                  active_color_attachment_index, active_color_formats,
                  active_depth_format, active_stencil_format);
          if (clear_pipeline) {
            MTL::RenderCommandEncoder* clear_encoder =
                ensure_transfer_encoder();
            if (clear_encoder) {
              MTL::DepthStencilState* no_depth_state =
                  GetTransferNoDepthStencilState();
              if (!no_depth_state) {
                continue;
              }
              clear_encoder->setRenderPipelineState(clear_pipeline);
              clear_encoder->setDepthStencilState(no_depth_state);
              if (clear_use_uint) {
                clear_encoder->setFragmentBytes(&uint_constants,
                                                sizeof(uint_constants), 0);
              } else {
                clear_encoder->setFragmentBytes(&float_constants,
                                                sizeof(float_constants), 0);
              }
              mark_encoder_mutation(
                  kDrawPassTransferEncoderMutationPipeline |
                  kDrawPassTransferEncoderMutationDepthStencil |
                  kDrawPassTransferEncoderMutationFragmentSlot0);
              Transfer::Rectangle clear_rect = *resolve_clear_rectangle;
              if (set_rect_viewport(clear_encoder, clear_rect)) {
                clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                              NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
    }

    if (transfer_encoder && !use_active_render_encoder) {
      transfer_encoder->endEncoding();
    }
  }
  return true;
}

MTL::Function* MetalRenderTargetCache::GetTransferRectVertexFunction() {
  if (transfer_rect_vertex_function_) {
    return transfer_rect_vertex_function_;
  }
  MTL::Library* library = GetOrCreateTransferLibrary();
  if (!library) {
    return nullptr;
  }
  transfer_rect_vertex_function_ = library->newFunction(
      NS::String::string("transfer_rect_vs", NS::UTF8StringEncoding));
  if (!transfer_rect_vertex_function_) {
    XELOGE(
        "MetalRenderTargetCache: the transfer library has no transfer_rect_vs");
  }
  return transfer_rect_vertex_function_;
}

bool MetalRenderTargetCache::TransferDrawsSamplesSeparately(
    EdramTransferShaderKey key) const {
  if (!cvars::metal_transfer_sample_mask) {
    return false;
  }
  if (key.dest_msaa_samples == xenos::MsaaSamples::k1X) {
    return false;
  }
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    return false;
  }
  return EdramTransferHostDepthIsCopy(key.mode) ||
         key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X;
}

MTL::Function* MetalRenderTargetCache::GetOrCreateTransferFragmentFunction(
    EdramTransferShaderKey key) {
  auto it = transfer_fragment_functions_.find(key);
  if (it != transfer_fragment_functions_.end()) {
    return it->second;
  }

  const EdramTransferModeInfo& mode_info =
      kEdramTransferModes[size_t(key.mode)];

  EdramTransferShaderOptions options;
  // The host depth buffer is declared with the pre-1.3 BufferBlock and Uniform
  // forms, like the dump shader's EDRAM buffer.
  options.spirv_version = 0x00010000;
  options.resolution_scale_x = draw_resolution_scale_x();
  options.resolution_scale_y = draw_resolution_scale_y();
  options.msaa_2x_attachments_supported = msaa_2x_supported_;
  if (EdramTransferSourceIsColor(key.mode)) {
    GetColorOwnershipTransferPixelFormat(
        xenos::ColorRenderTargetFormat(key.source_resource_format),
        &options.source_color_is_uint);
  }
  if (mode_info.output == EdramTransferOutput::kColor) {
    GetColorOwnershipTransferPixelFormat(
        xenos::ColorRenderTargetFormat(key.dest_resource_format),
        &options.dest_color_is_uint);
  }
  options.stencil_reference_output_supported =
      UseNativeStencilOutputInTransfers();
  const bool draws_samples_separately = TransferDrawsSamplesSeparately(key);
  options.sample_rate_shading_supported = !draws_samples_separately;
  // Metal has no pipeline-level sample mask, so the shader has to write the
  // one sample its draw covers, and the index rides in the push constants so
  // that one pipeline serves every sample.
  options.sample_mask_output = draws_samples_separately;
  options.sample_index_push_constant = draws_samples_separately;
  options.depth_float24_round = ::cvars::depth_float24_round;
  options.depth_float24_convert_in_pixel_shader =
      ::cvars::depth_float24_convert_in_pixel_shader;
  options.no_discard_stencil =
      ::cvars::no_discard_stencil_in_transfer_pipelines;
  options.fast_pitch_divmod = ::cvars::metal_transfer_fast_divmod;

  MTL::Function* function = nullptr;
  std::vector<uint32_t> spirv = BuildEdramTransferShaderSpirv(key, options);
  if (spirv.empty()) {
    XELOGE(
        "MetalRenderTargetCache: failed to emit the transfer shader 0x{:08X}",
        key.key);
  } else if (cvars::metal_transfer_spirv_cross) {
    std::string error;
    function = CompileTransferFragmentFunctionMsl(device_, spirv, &error);
    if (!function) {
      XELOGE(
          "MetalRenderTargetCache: failed to compile the MSL transfer shader "
          "0x{:08X}: {}",
          key.key, error);
    }
  } else {
    std::vector<uint8_t> dxil = SpirvToDxilCompiler::Translate(
        spirv.data(), spirv.size(), SpirvToDxilCompiler::Stage::kPixel);
    if (dxil.empty()) {
      XELOGE(
          "MetalRenderTargetCache: failed to translate the transfer shader "
          "0x{:08X}",
          key.key);
    } else {
      MetalShaderConversionResult conversion =
          command_processor_.metal_shader_converter().ConvertInternalGraphics(
              MetalShaderStage::kFragment, dxil);
      if (!conversion.success) {
        XELOGE(
            "MetalRenderTargetCache: failed to convert the transfer shader "
            "0x{:08X}: {}",
            key.key, conversion.error_message);
      } else {
        NS::Error* error = nullptr;
        dispatch_data_t data = dispatch_data_create(
            conversion.metallib.data(), conversion.metallib.size(), nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        MTL::Library* library = device_->newLibrary(data, &error);
        dispatch_release(data);
        if (!library) {
          XELOGE(
              "MetalRenderTargetCache: transfer shader 0x{:08X} metallib "
              "rejected: {}",
              key.key,
              error ? error->localizedDescription()->utf8String() : "unknown");
        } else {
          function = library->newFunction(NS::String::string(
              conversion.entry_point_name.c_str(), NS::UTF8StringEncoding));
          if (!function) {
            XELOGE(
                "MetalRenderTargetCache: transfer shader 0x{:08X} has no "
                "function named {}",
                key.key, conversion.entry_point_name);
          }
          library->release();
        }
      }
    }
  }
  // A failed key is cached as null so it is not retried on every transfer.
  transfer_fragment_functions_.emplace(key, function);
  return function;
}

MTL::RenderPipelineState* MetalRenderTargetCache::GetOrCreateTransferPipelines(
    const EdramTransferShaderKey& key, MTL::PixelFormat dest_format,
    bool dest_is_uint, bool native_stencil_output,
    uint32_t color_attachment_index,
    const TransferColorAttachmentFormats* color_attachment_formats,
    MTL::PixelFormat depth_attachment_format,
    MTL::PixelFormat stencil_attachment_format) {
  const EdramTransferModeInfo& mode_info =
      kEdramTransferModes[size_t(key.mode)];
  EdramTransferOutput output = mode_info.output;
  bool source_is_color = EdramTransferSourceIsColor(key.mode);
  bool has_host_depth = EdramTransferUsesHostDepth(key.mode);
  native_stencil_output =
      native_stencil_output && output == EdramTransferOutput::kDepth;

  TransferPipelineKey pipeline_key = {};
  pipeline_key.shader_key = key;
  pipeline_key.native_stencil_output = native_stencil_output ? 1u : 0u;
  if (color_attachment_formats) {
    pipeline_key.color_attachment_formats = *color_attachment_formats;
  }
  if (output == EdramTransferOutput::kColor) {
    if (color_attachment_index >= xenos::kMaxColorRenderTargets) {
      return nullptr;
    }
    pipeline_key.color_attachment_index = color_attachment_index;
    if (!color_attachment_formats) {
      pipeline_key.color_attachment_formats[color_attachment_index] =
          dest_format;
    }
    if (pipeline_key.color_attachment_formats[color_attachment_index] !=
        dest_format) {
      return nullptr;
    }
    pipeline_key.depth_attachment_format = depth_attachment_format;
    pipeline_key.stencil_attachment_format = stencil_attachment_format;
  } else {
    pipeline_key.depth_attachment_format =
        depth_attachment_format != MTL::PixelFormatInvalid
            ? depth_attachment_format
            : dest_format;
    if (pipeline_key.depth_attachment_format != dest_format) {
      return nullptr;
    }
    if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
      pipeline_key.stencil_attachment_format =
          stencil_attachment_format != MTL::PixelFormatInvalid
              ? stencil_attachment_format
              : dest_format;
      if (pipeline_key.stencil_attachment_format != dest_format) {
        return nullptr;
      }
    } else {
      pipeline_key.stencil_attachment_format = stencil_attachment_format;
    }
  }

  auto it = transfer_pipelines_.find(pipeline_key);
  if (it != transfer_pipelines_.end()) {
    return it->second;
  }

  MTL::Function* fragment_function = GetOrCreateTransferFragmentFunction(key);
  MTL::Function* vertex_function = GetTransferRectVertexFunction();
  if (!fragment_function || !vertex_function) {
    transfer_pipelines_.emplace(pipeline_key, nullptr);
    return nullptr;
  }

  NS::Error* error = nullptr;

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vertex_function);
  desc->setFragmentFunction(fragment_function);

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    color_attachment->setPixelFormat(pipeline_key.color_attachment_formats[i]);
    color_attachment->setWriteMask(
        output == EdramTransferOutput::kColor &&
                i == pipeline_key.color_attachment_index
            ? MTL::ColorWriteMaskAll
            : MTL::ColorWriteMaskNone);
  }
  desc->setDepthAttachmentPixelFormat(pipeline_key.depth_attachment_format);
  desc->setStencilAttachmentPixelFormat(pipeline_key.stencil_attachment_format);

  uint32_t sample_count = 1;
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
    sample_count = 2;
  } else if (key.dest_msaa_samples == xenos::MsaaSamples::k4X) {
    sample_count = 4;
  }
  desc->setSampleCount(sample_count);

  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();

  if (!pipeline) {
    XELOGE(
        "GetOrCreateTransferPipelines: failed to create pipeline (mode={}): {}",
        int(key.mode),
        error && error->localizedDescription()
            ? error->localizedDescription()->utf8String()
            : "unknown error");
    transfer_pipelines_.emplace(pipeline_key, nullptr);
    return nullptr;
  }

  transfer_pipelines_.emplace(pipeline_key, pipeline);

  return pipeline;
}

MTL::Library* MetalRenderTargetCache::GetOrCreateTransferLibrary() {
  if (transfer_library_) {
    return transfer_library_;
  }
  static const char kTransferLibrarySource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
  float4 position [[position]];
};

struct TransferClearColorFloatConstants {
  float4 color;
};

struct TransferClearColorUintConstants {
  uint4 color;
};

struct TransferClearDepthConstants {
  float4 depth;
};

struct TransferRectInstance {
  float2 origin;
  float2 size;
};

struct TransferVertexConstants {
  float2 pixel_to_ndc;
};

// The shared fragment shader derives everything from the fragment coordinate,
// so this only has to place the rectangle.
vertex VSOut transfer_rect_vs(uint vid [[vertex_id]], uint iid [[instance_id]],
                              constant TransferVertexConstants& constants
                                  [[buffer(0)]],
                              constant TransferRectInstance* instances
                                  [[buffer(1)]]) {
  float2 quad = float2(float(vid & 1), float(vid >> 1));
  TransferRectInstance inst = instances[iid];
  float2 pos_pixel = inst.origin + quad * inst.size;
  VSOut out;
  out.position = float4(pos_pixel.x * constants.pixel_to_ndc.x - 1.0f,
                        1.0f - pos_pixel.y * constants.pixel_to_ndc.y, 0.0f,
                        1.0f);
  return out;
}

vertex VSOut transfer_clear_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

fragment float4 transfer_clear_color_float_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorFloatConstants& constants [[buffer(0)]]) {
  return constants.color;
}

fragment uint4 transfer_clear_color_uint_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorUintConstants& constants [[buffer(0)]]) {
  return constants.color;
}

struct TransferDepthOut {
  float depth [[depth(any)]];
};

fragment TransferDepthOut transfer_clear_depth_ps(
    VSOut in [[stage_in]],
    constant TransferClearDepthConstants& constants [[buffer(0)]]) {
  TransferDepthOut out;
  out.depth = constants.depth.x;
  return out;
}
)METAL";

  NS::Error* error = nullptr;
  auto source_str =
      NS::String::string(kTransferLibrarySource, NS::UTF8StringEncoding);
  transfer_library_ = device_->newLibrary(source_str, nullptr, &error);
  if (!transfer_library_) {
    XELOGE("GetOrCreateTransferLibrary: failed to compile transfer library: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return transfer_library_;
}

MTL::RenderPipelineState*
MetalRenderTargetCache::GetOrCreateTransferClearPipeline(
    MTL::PixelFormat dest_format, bool dest_is_uint, bool is_depth,
    uint32_t sample_count, uint32_t color_attachment_index,
    const TransferColorAttachmentFormats* color_attachment_formats,
    MTL::PixelFormat depth_attachment_format,
    MTL::PixelFormat stencil_attachment_format) {
  TransferClearPipelineKey key = {};
  key.sample_count = sample_count ? sample_count : 1;
  key.dest_is_uint = dest_is_uint ? 1u : 0u;
  key.is_depth = is_depth ? 1u : 0u;
  if (color_attachment_formats) {
    key.color_attachment_formats = *color_attachment_formats;
  }
  if (is_depth) {
    key.depth_attachment_format =
        depth_attachment_format != MTL::PixelFormatInvalid
            ? depth_attachment_format
            : dest_format;
    if (key.depth_attachment_format != dest_format) {
      return nullptr;
    }
    if (dest_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        dest_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
      key.stencil_attachment_format =
          stencil_attachment_format != MTL::PixelFormatInvalid
              ? stencil_attachment_format
              : dest_format;
      if (key.stencil_attachment_format != dest_format) {
        return nullptr;
      }
    } else {
      key.stencil_attachment_format = stencil_attachment_format;
    }
  } else {
    if (color_attachment_index >= xenos::kMaxColorRenderTargets) {
      return nullptr;
    }
    key.color_attachment_index = color_attachment_index;
    if (!color_attachment_formats) {
      key.color_attachment_formats[color_attachment_index] = dest_format;
    }
    if (key.color_attachment_formats[color_attachment_index] != dest_format) {
      return nullptr;
    }
    key.depth_attachment_format = depth_attachment_format;
    key.stencil_attachment_format = stencil_attachment_format;
  }
  auto it = transfer_clear_pipelines_.find(key);
  if (it != transfer_clear_pipelines_.end()) {
    return it->second;
  }

  MTL::Library* lib = GetOrCreateTransferLibrary();
  if (!lib) {
    return nullptr;
  }

  auto vs_name =
      NS::String::string("transfer_clear_vs", NS::UTF8StringEncoding);
  const char* ps_name_cstr = nullptr;
  if (is_depth) {
    ps_name_cstr = "transfer_clear_depth_ps";
  } else {
    ps_name_cstr = dest_is_uint ? "transfer_clear_color_uint_ps"
                                : "transfer_clear_color_float_ps";
  }
  auto ps_name = NS::String::string(ps_name_cstr, NS::UTF8StringEncoding);

  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    XELOGE(
        "GetOrCreateTransferClearPipeline: missing transfer clear functions");
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->setSampleCount(key.sample_count);

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    color_attachment->setPixelFormat(key.color_attachment_formats[i]);
    color_attachment->setWriteMask(!is_depth && i == key.color_attachment_index
                                       ? MTL::ColorWriteMaskAll
                                       : MTL::ColorWriteMaskNone);
  }
  desc->setDepthAttachmentPixelFormat(key.depth_attachment_format);
  desc->setStencilAttachmentPixelFormat(key.stencil_attachment_format);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    XELOGE("GetOrCreateTransferClearPipeline: failed to create pipeline: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
    return nullptr;
  }

  transfer_clear_pipelines_.emplace(key, pipeline);
  return pipeline;
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyTexture(
    MTL::PixelFormat format, uint32_t sample_count) {
  if (!device_) {
    return nullptr;
  }
  MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(1);
  desc->setHeight(1);
  desc->setPixelFormat(format);
  desc->setTextureType(sample_count > 1 ? MTL::TextureType2DMultisample
                                        : MTL::TextureType2D);
  desc->setSampleCount(sample_count ? sample_count : 1);
  MTL::TextureUsage usage = MTL::TextureUsageShaderRead;
  if (format == MTL::PixelFormatDepth32Float_Stencil8 ||
      format == MTL::PixelFormatDepth24Unorm_Stencil8) {
    usage |= MTL::TextureUsagePixelFormatView;
  }
  desc->setUsage(usage);
  desc->setStorageMode(MTL::StorageModePrivate);
  MTL::Texture* tex = device_->newTexture(desc);
  desc->release();
  return tex;
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyColorFloatTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_color_float_[index]) {
    transfer_dummy_color_float_[index] =
        GetTransferDummyTexture(MTL::PixelFormatRGBA8Unorm, sample_count);
  }
  return transfer_dummy_color_float_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyColorUintTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_color_uint_[index]) {
    transfer_dummy_color_uint_[index] =
        GetTransferDummyTexture(MTL::PixelFormatRGBA8Uint, sample_count);
  }
  return transfer_dummy_color_uint_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyDepthTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_depth_[index]) {
    transfer_dummy_depth_[index] = GetTransferDummyTexture(
        MTL::PixelFormatDepth32Float_Stencil8, sample_count);
  }
  return transfer_dummy_depth_[index];
}

MTL::Texture* MetalRenderTargetCache::GetTransferDummyStencilTexture(
    uint32_t sample_count) {
  size_t index = sample_count >= 4 ? 2 : (sample_count == 2 ? 1 : 0);
  if (!transfer_dummy_stencil_[index]) {
    MTL::Texture* depth_tex = GetTransferDummyDepthTexture(sample_count);
    if (!depth_tex) {
      return nullptr;
    }
    transfer_dummy_stencil_[index] =
        depth_tex->newTextureView(MTL::PixelFormatX32_Stencil8);
  }
  return transfer_dummy_stencil_[index];
}

MTL::Buffer* MetalRenderTargetCache::GetTransferDummyBuffer() {
  if (!transfer_dummy_buffer_ && device_) {
    transfer_dummy_buffer_ =
        device_->newBuffer(sizeof(uint32_t), MTL::ResourceStorageModeShared);
    if (transfer_dummy_buffer_) {
    }
    if (transfer_dummy_buffer_) {
      std::memset(transfer_dummy_buffer_->contents(), 0, sizeof(uint32_t));
    }
  }
  return transfer_dummy_buffer_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferDepthStencilState(
    bool depth_write) {
  if (transfer_depth_state_) {
    return transfer_depth_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(::cvars::depth_transfer_not_equal_test
                                    ? MTL::CompareFunctionNotEqual
                                    : MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(depth_write);
  transfer_depth_state_ = device_->newDepthStencilState(desc);
  desc->release();
  return transfer_depth_state_;
}

MTL::DepthStencilState*
MetalRenderTargetCache::GetTransferDepthAndStencilOutputState() {
  if (transfer_depth_stencil_output_state_) {
    return transfer_depth_stencil_output_state_;
  }
  bool not_equal_test = ::cvars::depth_transfer_not_equal_test;
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(not_equal_test ? MTL::CompareFunctionNotEqual
                                               : MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(true);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  // Always, not not-equal, so a differing stencil doesn't suppress the depth
  // write - and with the not-equal depth test, replacing on depth failure so
  // matching depth still gets its stencil written.
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(not_equal_test
                                        ? MTL::StencilOperationReplace
                                        : MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_depth_stencil_output_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_depth_stencil_output_state_;
}

MTL::DepthStencilState*
MetalRenderTargetCache::GetTransferNoDepthStencilState() {
  if (transfer_depth_state_none_) {
    return transfer_depth_state_none_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  transfer_depth_state_none_ = device_->newDepthStencilState(desc);
  desc->release();
  return transfer_depth_state_none_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferDepthClearState() {
  if (transfer_depth_clear_state_) {
    return transfer_depth_clear_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(true);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_depth_clear_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_depth_clear_state_;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferStencilClearState() {
  if (transfer_stencil_clear_state_) {
    return transfer_stencil_clear_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_stencil_clear_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_stencil_clear_state_;
}

bool MetalRenderTargetCache::UseNativeStencilOutputInTransfers() const {
  return ::cvars::native_stencil_value_output && native_stencil_output_probed_;
}

bool MetalRenderTargetCache::ProbeNativeStencilOutputSupport() {
  // Metal has no query for fragment stencil output, and it decides which draws
  // a stencil transfer needs, so build a pipeline that uses it up front rather
  // than discovering it half way through a batch.
  static const char kProbeSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

vertex float4 xe_stencil_probe_vs() {
  return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

struct XeStencilProbeOut {
  uint stencil [[stencil]];
};

fragment XeStencilProbeOut xe_stencil_probe_ps() {
  XeStencilProbeOut out;
  out.stencil = 0u;
  return out;
}
)METAL";

  NS::Error* error = nullptr;
  MTL::Library* lib = device_->newLibrary(
      NS::String::string(kProbeSource, NS::UTF8StringEncoding), nullptr,
      &error);
  if (!lib) {
    return false;
  }
  MTL::Function* vs = lib->newFunction(
      NS::String::string("xe_stencil_probe_vs", NS::UTF8StringEncoding));
  MTL::Function* ps = lib->newFunction(
      NS::String::string("xe_stencil_probe_ps", NS::UTF8StringEncoding));
  MTL::RenderPipelineState* pipeline = nullptr;
  if (vs && ps) {
    MTL::PixelFormat depth_format =
        GetDepthPixelFormat(xenos::DepthRenderTargetFormat::kD24S8);
    MTL::RenderPipelineDescriptor* desc =
        MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vs);
    desc->setFragmentFunction(ps);
    desc->setDepthAttachmentPixelFormat(depth_format);
    desc->setStencilAttachmentPixelFormat(depth_format);
    pipeline = device_->newRenderPipelineState(desc, &error);
    desc->release();
  }
  if (vs) {
    vs->release();
  }
  if (ps) {
    ps->release();
  }
  lib->release();
  if (!pipeline) {
    return false;
  }
  pipeline->release();
  return true;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetTransferStencilBitState(
    uint32_t bit) {
  if (bit >= 8) {
    return nullptr;
  }
  if (transfer_stencil_bit_states_[bit]) {
    return transfer_stencil_bit_states_[bit];
  }
  uint32_t mask = uint32_t(1) << bit;
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(uint32_t(mask));
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_stencil_bit_states_[bit] = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_stencil_bit_states_[bit];
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
