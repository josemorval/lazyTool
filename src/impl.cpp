// impl.cpp — single-file header library implementations
// Only compiled once. Never include these defines elsewhere.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4267)
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
