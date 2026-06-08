/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHADER_ABI_H_
#define XENIA_GPU_SHADER_ABI_H_

#include <cstdint>

#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/dxbc_shader_translator.h"

namespace xe {
namespace gpu {

// Shared shader ABI concepts. These currently alias the DXBC/MSC definitions so
// native MSL can move off DXBC-owned names without changing layout or behavior.
using ShaderModification = DxbcShaderTranslator::Modification;
using ShaderSystemConstants = DxbcShaderTranslator::SystemConstants;
using ShaderCbufferRegister = DxbcShaderTranslator::CbufferRegister;

using ShaderTextureBinding = DxbcShader::TextureBinding;
using ShaderSamplerBinding = DxbcShader::SamplerBinding;
using ShaderFetchConstantDwordMask = DxbcShader::FetchConstantDwordMask;
using ShaderTextureSignComponentMasks = DxbcShader::TextureSignComponentMasks;
using ShaderTranslationMetadata = DxbcShader::TranslationMetadata;

inline constexpr uint32_t kShaderSysFlagSharedMemoryIsUAVShift =
    DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV_Shift;
inline constexpr uint32_t kShaderSysFlagXYDividedByWShift =
    DxbcShaderTranslator::kSysFlag_XYDividedByW_Shift;
inline constexpr uint32_t kShaderSysFlagZDividedByWShift =
    DxbcShaderTranslator::kSysFlag_ZDividedByW_Shift;
inline constexpr uint32_t kShaderSysFlagWNotReciprocalShift =
    DxbcShaderTranslator::kSysFlag_WNotReciprocal_Shift;
inline constexpr uint32_t kShaderSysFlagPrimitivePolygonalShift =
    DxbcShaderTranslator::kSysFlag_PrimitivePolygonal_Shift;
inline constexpr uint32_t kShaderSysFlagPrimitiveLineShift =
    DxbcShaderTranslator::kSysFlag_PrimitiveLine_Shift;
inline constexpr uint32_t kShaderSysFlagDepthFloat24Shift =
    DxbcShaderTranslator::kSysFlag_DepthFloat24_Shift;
inline constexpr uint32_t kShaderSysFlagAlphaPassIfLessShift =
    DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
inline constexpr uint32_t kShaderSysFlagAlphaPassIfEqualShift =
    DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual_Shift;
inline constexpr uint32_t kShaderSysFlagAlphaPassIfGreaterShift =
    DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater_Shift;
inline constexpr uint32_t kShaderSysFlagConvertColor0ToGammaShift =
    DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma_Shift;
inline constexpr uint32_t kShaderSysFlagConvertColor1ToGammaShift =
    DxbcShaderTranslator::kSysFlag_ConvertColor1ToGamma_Shift;
inline constexpr uint32_t kShaderSysFlagConvertColor2ToGammaShift =
    DxbcShaderTranslator::kSysFlag_ConvertColor2ToGamma_Shift;
inline constexpr uint32_t kShaderSysFlagConvertColor3ToGammaShift =
    DxbcShaderTranslator::kSysFlag_ConvertColor3ToGamma_Shift;
inline constexpr uint32_t kShaderSysFlagROVDepthStencilShift =
    DxbcShaderTranslator::kSysFlag_ROVDepthStencil_Shift;
inline constexpr uint32_t kShaderSysFlagROVDepthPassIfLessShift =
    DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
inline constexpr uint32_t kShaderSysFlagROVDepthPassIfEqualShift =
    DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual_Shift;
inline constexpr uint32_t kShaderSysFlagROVDepthPassIfGreaterShift =
    DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater_Shift;
inline constexpr uint32_t kShaderSysFlagROVDepthWriteShift =
    DxbcShaderTranslator::kSysFlag_ROVDepthWrite_Shift;
inline constexpr uint32_t kShaderSysFlagROVStencilTestShift =
    DxbcShaderTranslator::kSysFlag_ROVStencilTest_Shift;
inline constexpr uint32_t kShaderSysFlagROVDepthStencilEarlyWriteShift =
    DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite_Shift;
inline constexpr uint32_t kShaderSysFlagCount =
    DxbcShaderTranslator::kSysFlag_Count;

inline constexpr uint32_t kShaderSysFlagSharedMemoryIsUAV =
    DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
inline constexpr uint32_t kShaderSysFlagXYDividedByW =
    DxbcShaderTranslator::kSysFlag_XYDividedByW;
inline constexpr uint32_t kShaderSysFlagZDividedByW =
    DxbcShaderTranslator::kSysFlag_ZDividedByW;
inline constexpr uint32_t kShaderSysFlagWNotReciprocal =
    DxbcShaderTranslator::kSysFlag_WNotReciprocal;
inline constexpr uint32_t kShaderSysFlagPrimitivePolygonal =
    DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
inline constexpr uint32_t kShaderSysFlagPrimitiveLine =
    DxbcShaderTranslator::kSysFlag_PrimitiveLine;
inline constexpr uint32_t kShaderSysFlagDepthFloat24 =
    DxbcShaderTranslator::kSysFlag_DepthFloat24;
inline constexpr uint32_t kShaderSysFlagAlphaPassIfLess =
    DxbcShaderTranslator::kSysFlag_AlphaPassIfLess;
inline constexpr uint32_t kShaderSysFlagAlphaPassIfEqual =
    DxbcShaderTranslator::kSysFlag_AlphaPassIfEqual;
inline constexpr uint32_t kShaderSysFlagAlphaPassIfGreater =
    DxbcShaderTranslator::kSysFlag_AlphaPassIfGreater;
inline constexpr uint32_t kShaderSysFlagConvertColor0ToGamma =
    DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma;
inline constexpr uint32_t kShaderSysFlagConvertColor1ToGamma =
    DxbcShaderTranslator::kSysFlag_ConvertColor1ToGamma;
inline constexpr uint32_t kShaderSysFlagConvertColor2ToGamma =
    DxbcShaderTranslator::kSysFlag_ConvertColor2ToGamma;
inline constexpr uint32_t kShaderSysFlagConvertColor3ToGamma =
    DxbcShaderTranslator::kSysFlag_ConvertColor3ToGamma;
inline constexpr uint32_t kShaderSysFlagROVDepthStencil =
    DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
inline constexpr uint32_t kShaderSysFlagROVDepthPassIfLess =
    DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess;
inline constexpr uint32_t kShaderSysFlagROVDepthPassIfEqual =
    DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual;
inline constexpr uint32_t kShaderSysFlagROVDepthPassIfGreater =
    DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
inline constexpr uint32_t kShaderSysFlagROVDepthWrite =
    DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
inline constexpr uint32_t kShaderSysFlagROVStencilTest =
    DxbcShaderTranslator::kSysFlag_ROVStencilTest;
inline constexpr uint32_t kShaderSysFlagROVDepthStencilEarlyWrite =
    DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SHADER_ABI_H_
