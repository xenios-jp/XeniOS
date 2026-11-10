// The rgb10f-to-rgba16 2x and 4x average direct resolve, also refreshing the
// destination's texture-cache image (see
// resolve_full_64bpp_rgb10f_rgba16_msaa_direct.cs.slang).
// XE_MSL_ONLY
#define XE_RESOLVE_DIRECT_REFRESH 1
#include "resolve_full_64bpp_rgb10f_rgba16_msaa_direct.cs.slang"
