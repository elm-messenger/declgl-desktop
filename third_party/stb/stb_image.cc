// Single TU that instantiates stb_image's implementation. The header
// is exposed to consumers; the implementation lives only here.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG  // friendlier `stbi_failure_reason()` strings
// We do want the C++ default malloc/realloc/free hooks. PNG/JPEG/BMP/TGA/PSD
// are all enabled (the formats most likely to appear in 2D-game assets).
#include "stb_image.h"
