// impl.cpp — single-file header library implementations
// Only compiled once. Never include these defines elsewhere.

// This file centralizes the one-definition implementation toggles for the
// header-only third-party libraries used by the project.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#undef CGLTF_IMPLEMENTATION

#ifndef LAZYTOOL_PLAYER_ONLY
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4267)
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#undef NANOSVG_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif
