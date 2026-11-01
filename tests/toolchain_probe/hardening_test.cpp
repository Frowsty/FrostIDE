// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe: the standard library is hardened where PLAN.md section 2
// ("C++23/26 policy") requires it, in Debug and in the sanitizer (verification)
// builds. cmake/FrostCompilerOptions.cmake defines the hardening macros;
// tests/toolchain_probe/CMakeLists.txt defines FROST_EXPECT_STL_HARDENING for the
// same configurations, so a build that lost the hardening define fails to compile
// here instead of silently running its tests without bounds checks.

#include <vector> // Any container header; it defines the library's identification macros.

#if defined(FROST_EXPECT_STL_HARDENING)
#if defined(_MSVC_STL_VERSION)
#if !defined(_MSVC_STL_HARDENING) || !_MSVC_STL_HARDENING
#error "this configuration requires _MSVC_STL_HARDENING=1, but the MSVC STL is not hardened"
#endif
#elif defined(__GLIBCXX__)
#if !defined(_GLIBCXX_ASSERTIONS)
#error "this configuration requires _GLIBCXX_ASSERTIONS, but libstdc++ assertions are off"
#endif
#else
#error "unknown standard library: teach this probe and FrostCompilerOptions.cmake its hardening macro"
#endif
#endif
