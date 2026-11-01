// XE_MSL_ONLY
// texture_load_r5g6b5_b5g6r5 writing a level of the host 2D array image directly through
// an integer view of its 2-byte host block, rather than a buffer to copy from.
#define XE_TEXTURE_LOAD_IMAGE_TYPE uint4
#define XE_TEXTURE_LOAD_IMAGE_FORMAT "rgba32ui"
#define XE_TEXTURE_LOAD_IMAGE_BLOCK_BYTES 2
#include "texture_load_r5g6b5_b5g6r5.cs.slang"
