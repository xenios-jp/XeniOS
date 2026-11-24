// Transfer rectangle placement, resolve clear, and native stencil-output probe
// shaders for MetalTransferEncoder.
// Matches TransferVertexConstants, TransferRectInstance, and
// TransferClear*Constants in src/xenia/gpu/metal/metal_transfer_encoder.h.

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

// Startup probe for fragment [[stencil]] output support.
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
