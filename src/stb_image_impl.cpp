// stb_image_impl.cpp - the single translation unit that instantiates stb_image.
//
// stb ships as a header that emits its definitions only where
// STB_IMAGE_IMPLEMENTATION is defined, so exactly one source file may do it.
// Keeping that here rather than inside a consumer means a second consumer can
// be added by including the header alone.
//
// src/igdb_cover_library.cpp and src/bezel_library.cpp both consume it by
// including the header alone.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
// Bezels are 1920x1080 and IGDB covers smaller still, so anything larger is
// not artwork this program asked for.
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"
