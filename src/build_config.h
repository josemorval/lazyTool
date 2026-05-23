#pragma once

// Central build-profile switches. build.bat defines one of
// LAZYTOOL_CONFIG_FAST / LAZYTOOL_CONFIG_PROFILE / LAZYTOOL_CONFIG_RELEASE.
// Keep these macros numeric so code can use #if checks instead of fragile
// string comparisons against LAZYTOOL_BUILD_CONFIG.

#ifndef LAZYTOOL_BUILD_CONFIG
#  if defined(LAZYTOOL_CONFIG_RELEASE)
#    define LAZYTOOL_BUILD_CONFIG "release"
#  elif defined(LAZYTOOL_CONFIG_PROFILE)
#    define LAZYTOOL_BUILD_CONFIG "profile"
#  elif defined(LAZYTOOL_CONFIG_FAST)
#    define LAZYTOOL_BUILD_CONFIG "fast"
#  else
#    define LAZYTOOL_BUILD_CONFIG "custom"
#  endif
#endif

#ifndef LAZYTOOL_ENABLE_PROFILER
#  if defined(LAZYTOOL_CONFIG_RELEASE) || defined(LAZYTOOL_PLAYER_ONLY)
#    define LAZYTOOL_ENABLE_PROFILER 0
#  else
#    define LAZYTOOL_ENABLE_PROFILER 1
#  endif
#endif

#ifndef LAZYTOOL_ENABLE_D3D11_VALIDATION
#  if defined(LAZYTOOL_CONFIG_RELEASE) || defined(LAZYTOOL_PLAYER_ONLY)
#    define LAZYTOOL_ENABLE_D3D11_VALIDATION 0
#  else
#    define LAZYTOOL_ENABLE_D3D11_VALIDATION 1
#  endif
#endif

#ifndef LAZYTOOL_ENABLE_SHADER_BINDING_WARNINGS
#  if defined(LAZYTOOL_CONFIG_RELEASE) || defined(LAZYTOOL_PLAYER_ONLY)
#    define LAZYTOOL_ENABLE_SHADER_BINDING_WARNINGS 0
#  else
#    define LAZYTOOL_ENABLE_SHADER_BINDING_WARNINGS 1
#  endif
#endif

#ifndef LAZYTOOL_ENABLE_DEBUG_OVERLAYS
#  if defined(LAZYTOOL_CONFIG_RELEASE) || defined(LAZYTOOL_PLAYER_ONLY)
#    define LAZYTOOL_ENABLE_DEBUG_OVERLAYS 0
#  else
#    define LAZYTOOL_ENABLE_DEBUG_OVERLAYS 1
#  endif
#endif

#ifndef LAZYTOOL_ENABLE_BASIC_MONITORING
#  define LAZYTOOL_ENABLE_BASIC_MONITORING 1
#endif
