// XE_MSL_ONLY
// The 64bpb loader writing the texels of a tiled base image straight into it
// through its RG32Uint view, which preserves each guest half-float bit pattern.
#define XE_TEXTURE_LOAD_IMAGE_TYPE uint2
#define XE_TEXTURE_LOAD_IMAGE_FORMAT "rg32ui"
#define XE_TEXTURE_LOAD_64BPB_TRANSFORM(blocks) (blocks)
#include "texture_load_64bpb.xesli"
