// XE_MSL_ONLY
// texture_load_r11g11b10_rgba16 writing a level of the host 2D array image directly through
// an integer view of its 8-byte host block, rather than a buffer to copy from.
#define XE_TEXTURE_LOAD_IMAGE_TYPE uint4
#define XE_TEXTURE_LOAD_IMAGE_FORMAT "rgba32ui"
#define XE_TEXTURE_LOAD_IMAGE_BLOCK_BYTES 8
#include "texture_load_r11g11b10_rgba16.cs.slang"
