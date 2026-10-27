// XE_MSL_ONLY
// The 32bpb loader writing the texels of a tiled base image straight into it
// through its R32Uint view, which preserves each loaded word.
#define XE_TEXTURE_LOAD_IMAGE_TYPE uint
#define XE_TEXTURE_LOAD_IMAGE_FORMAT "r32ui"
#define XE_TEXTURE_LOAD_32BPB_TRANSFORM(blocks) (blocks)
#include "texture_load_32bpb.xesli"
