// XE_MSL_ONLY
// texture_load_128bpb writing a level of the host 2D array image directly through
// an integer view of its 16-byte host block, rather than a buffer to copy from.
#define XE_TEXTURE_LOAD_IMAGE_TYPE uint4
#define XE_TEXTURE_LOAD_IMAGE_FORMAT "rgba32ui"
#define XE_TEXTURE_LOAD_IMAGE_BLOCK_BYTES 16
#include "texture_load_128bpb.cs.slang"
